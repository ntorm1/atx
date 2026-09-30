#include "atx/engine/factory/ic_screen.hpp"
#include "atx/engine/factory/ic_research.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <xsimd/xsimd.hpp>

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/eval/hac.hpp"
#include "atx/engine/parallel/det_pool.hpp"

namespace atx::engine::factory {
namespace ic_screen_detail {
struct Cache {
  IcScreenConfig config;
  ResearchIcOptions options{4,false};
  std::array<ResearchIcCoverage,4> coverage{};
  atx::usize dates{};
  atx::usize instruments{};
  atx::usize rows{};
  atx::u64 bytes{};
  std::array<atx::usize, 4> active{};
  std::array<std::vector<atx::f64>, 4> labels;
  std::array<std::vector<atx::f64>, 4> ranks;
  std::array<std::vector<atx::usize>, 4> names;
  std::vector<atx::usize> eligible;
};

struct RowScratch {
  std::vector<atx::f64> x, y, xr, yr;
  std::vector<atx::usize> order, ranked_names;
  std::array<atx::u64, 4> paired{};
};
struct Scratch {
  // Binding prevents accidental reuse with a different window/configuration.
  std::shared_ptr<const Cache> cache;
  atx::u64 bytes{};
  std::vector<RowScratch> rows;
  std::vector<atx::f64> influence;
  std::array<std::vector<atx::f64>, 4> pearson, rank;
};
} // namespace ic_screen_detail

namespace {
using atx::f64;
using atx::usize;
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using ic_screen_detail::Cache;
using ic_screen_detail::RowScratch;
using ic_screen_detail::Scratch;
using Batch = xsimd::batch<f64>;
constexpr f64 kNan = std::numeric_limits<f64>::quiet_NaN();
constexpr f64 kMinCoverage = 0.8;
constexpr f64 kDirectionalSe = 1.0; // weak evidence passes, even below the effect floor

[[nodiscard]] bool checked_add_bytes(atx::u64& total, usize count, atx::u64 width) noexcept {
  const auto n = static_cast<atx::u64>(count);
  if (n > (std::numeric_limits<atx::u64>::max() - total) / width) return false;
  total += n * width;
  return true;
}

// Stable average ranks: ties (including signed zeros) have identical ranks.
// The index tie-breaker makes sorting deterministic without allocation.
void rank_values(std::span<const f64> values, std::span<f64> ranks,
                 std::span<usize> order) noexcept {
  std::fill(ranks.begin(), ranks.end(), kNan);
  usize n = 0;
  for (usize i = 0; i < values.size(); ++i) {
    if (std::isfinite(values[i])) order[n++] = i;
  }
  auto ids = order.first(n);
  std::sort(ids.begin(), ids.end(), [&](usize a, usize b) {
    return values[a] < values[b] || (values[a] == values[b] && a < b);
  });
  for (usize begin = 0; begin < n;) {
    usize end = begin + 1U;
    while (end < n && values[ids[end]] == values[ids[begin]]) ++end;
    const f64 rank = (static_cast<f64>(begin) + static_cast<f64>(end - 1U)) * 0.5;
    for (usize j = begin; j < end; ++j) ranks[ids[j]] = rank;
    begin = end;
  }
}

// All operands are paired finite observations. Scale BEFORE summation, then
// center in a second SIMD pass: huge finite levels cannot overflow x*x, and
// subtraction of two nearly equal raw second moments cannot invent variance.
[[nodiscard]] f64 correlation(std::span<const f64> x, std::span<const f64> y) noexcept {
  const usize n = x.size();
  if (n < 3U) return kNan;
  constexpr usize width = Batch::size;
  const usize stop = n - n % width;
  Batch max_x(0.0), max_y(0.0);
  for (usize i = 0; i < stop; i += width) {
    max_x = xsimd::max(max_x, xsimd::abs(Batch::load_unaligned(x.data() + i)));
    max_y = xsimd::max(max_y, xsimd::abs(Batch::load_unaligned(y.data() + i)));
  }
  f64 sx = xsimd::reduce_max(max_x), sy = xsimd::reduce_max(max_y);
  for (usize i = stop; i < n; ++i) {
    sx = std::max(sx, std::abs(x[i])); sy = std::max(sy, std::abs(y[i]));
  }
  if (!(sx > 0.0) || !(sy > 0.0)) return kNan;
  Batch sum_x(0.0), sum_y(0.0);
  for (usize i = 0; i < stop; i += width) {
    sum_x += Batch::load_unaligned(x.data() + i) / Batch(sx);
    sum_y += Batch::load_unaligned(y.data() + i) / Batch(sy);
  }
  f64 mx = xsimd::reduce_add(sum_x), my = xsimd::reduce_add(sum_y);
  for (usize i = stop; i < n; ++i) { mx += x[i] / sx; my += y[i] / sy; }
  mx /= static_cast<f64>(n); my /= static_cast<f64>(n);
  Batch xx(0.0), yy(0.0), xy(0.0);
  for (usize i = 0; i < stop; i += width) {
    const Batch dx = Batch::load_unaligned(x.data() + i) / Batch(sx) - Batch(mx);
    const Batch dy = Batch::load_unaligned(y.data() + i) / Batch(sy) - Batch(my);
    xx += dx * dx; yy += dy * dy; xy += dx * dy;
  }
  f64 vx = xsimd::reduce_add(xx), vy = xsimd::reduce_add(yy), cov = xsimd::reduce_add(xy);
  for (usize i = stop; i < n; ++i) {
    const f64 dx = x[i] / sx - mx, dy = y[i] / sy - my;
    vx += dx * dx; vy += dy * dy; cov += dx * dy;
  }
  const f64 flat = static_cast<f64>(n) * 64.0 * std::numeric_limits<f64>::epsilon() *
                   std::numeric_limits<f64>::epsilon();
  if (!(vx > flat) || !(vy > flat)) return kNan;
  const f64 rho = (cov / std::sqrt(vx)) / std::sqrt(vy);
  return std::isfinite(rho) ? std::clamp(rho, -1.0, 1.0) : kNan;
}

[[nodiscard]] atx::core::Status validate(const alpha::Panel& panel,
                                        const IcScreenConfig& cfg,
                                        std::span<const atx::u8> member,
                                        std::span<const atx::u32> bad,
                                        usize active_horizons) {
  if (cfg.rule != IcScreenRule::DisabledV1 && cfg.rule != IcScreenRule::ConservativeV2 &&
      cfg.rule != IcScreenRule::EquivalenceV3)
    return Err(ErrorCode::InvalidArgument, "IC screen: unknown rule");
  if (active_horizons==0 || active_horizons>4)
    return Err(ErrorCode::InvalidArgument,"IC research: active horizon count");
  if (cfg.rule == IcScreenRule::DisabledV1) return Ok();
  if (panel.instruments() == 0 || panel.dates() == 0 ||
      panel.dates() > std::numeric_limits<usize>::max() / panel.instruments())
    return Err(ErrorCode::InvalidArgument, "IC screen: invalid panel geometry");
  const usize cells = panel.dates() * panel.instruments();
  if ((!member.empty() && member.size() != cells) || (!bad.empty() && bad.size() != cells))
    return Err(ErrorCode::InvalidArgument, "IC screen: membership/guard shape mismatch");
  if (cfg.execution_delay < 1U || cfg.min_names < 3U || cfg.min_dates < 8U ||
      !std::isfinite(cfg.practical_abs_ic) || cfg.practical_abs_ic <= 0.0 ||
      cfg.practical_abs_ic >= 1.0 || !std::isfinite(cfg.confidence_multiplier) ||
      cfg.confidence_multiplier < 3.0 || cfg.max_cache_bytes == 0U)
    return Err(ErrorCode::InvalidArgument, "IC screen: invalid conservative configuration");
  for (usize k = 0; k < active_horizons; ++k) {
    const usize h = cfg.horizons[k];
    if (h == 0U || h > (std::numeric_limits<usize>::max() - 1U) / 2U ||
        h > std::numeric_limits<usize>::max() - cfg.execution_delay ||
        (k > 0U && h <= cfg.horizons[k - 1U]))
      return Err(ErrorCode::InvalidArgument, "IC screen: horizons must be positive, ordered and bounded");
  }
  const usize end = cfg.window_end == 0U ? panel.dates() : cfg.window_end;
  const usize maturity = cfg.maturity_end == 0U ? end : cfg.maturity_end;
  if (end > panel.dates() || cfg.window_begin > end || maturity > panel.dates())
    return Err(ErrorCode::InvalidArgument, "IC screen: invalid training/maturity window");
  if (!member.empty()) {
    const usize cutoff = std::min(end, maturity);
    const usize first_lag = cfg.execution_delay + cfg.horizons.front();
    const usize decision_end = cutoff > first_lag ? std::max(cfg.window_begin, cutoff - first_lag) : cfg.window_begin;
    const auto visible = member.subspan(cfg.window_begin * panel.instruments(),
                                       (decision_end - cfg.window_begin) * panel.instruments());
    if (std::any_of(visible.begin(), visible.end(), [](atx::u8 x) { return x > 1U; }))
      return Err(ErrorCode::InvalidArgument, "IC screen: membership must be binary");
  }
  for (usize d = 1; !bad.empty() && d < std::min(end, maturity); ++d) {
    for (usize i = 0; i < panel.instruments(); ++i) {
      if (bad[d * panel.instruments() + i] < bad[(d - 1U) * panel.instruments() + i])
        return Err(ErrorCode::InvalidArgument, "IC screen: excluded-return prefix decreases");
    }
  }
  return Ok();
}

// Preserve calendar spacing despite missing cross-sections. The HAC influence
// series is I_t*(IC_t - observed_mean); scale its mean SE by T/n_observed.
// Compressing missing dates would incorrectly turn distant observations into
// adjacent pairs and alter the overlap correction.
[[nodiscard]] IcScreenEstimate estimate(std::span<const f64> daily, usize horizon,
                                        const IcScreenConfig& cfg,
                                        std::span<f64> influence) noexcept {
  IcScreenEstimate out;
  out.calendar_dates = daily.size();
  f64 sum = 0.0;
  for (const f64 v : daily) if (std::isfinite(v)) { sum += v; ++out.valid_dates; }
  if (out.valid_dates == 0U) return out;
  out.mean = sum / static_cast<f64>(out.valid_dates);
  out.hac_lag = std::max(horizon * 2U, eval::hac::newey_west_rule_of_thumb_lag(daily.size()));
  if (out.valid_dates < cfg.min_dates || daily.size() <= out.hac_lag + 1U ||
      static_cast<f64>(out.valid_dates) < kMinCoverage * static_cast<f64>(daily.size())) return out;
  // Additional conservative heuristic: four fixed chronological segment means.
  // This is NOT a calibrated guarantee of regime-specific alpha recall. Sparse
  // segments cannot certify a practical null under either active rule.
  for (usize q = 0; q < 4U; ++q) {
    const usize begin = daily.size() / 4U * q + (daily.size() % 4U) * q / 4U;
    const usize end = daily.size() / 4U * (q + 1U) + (daily.size() % 4U) * (q + 1U) / 4U;
    f64 segment = 0.0; usize count = 0;
    for (usize d = begin; d < end; ++d) if (std::isfinite(daily[d])) { segment += daily[d]; ++count; }
    if (count < std::max(usize{2}, cfg.min_dates / 4U) ||
        static_cast<f64>(count) < kMinCoverage * static_cast<f64>(end - begin)) return out;
    out.max_segment_abs_ic = std::max(out.max_segment_abs_ic, std::abs(segment / static_cast<f64>(count)));
  }
  auto work = influence.first(daily.size());
  for (usize d = 0; d < daily.size(); ++d)
    work[d] = std::isfinite(daily[d]) ? daily[d] - out.mean : 0.0;
  const auto hac = eval::hac::mean_inference(work, eval::hac::Kernel::BartlettV1, out.hac_lag, true);
  if (!hac.defined) return out;
  out.standard_error = hac.se * static_cast<f64>(daily.size()) / static_cast<f64>(out.valid_dates);
  // A second no-autocorrelation bound prevents estimated negative serial
  // covariance from making a noisy signal look spuriously precise.
  const auto iid = eval::hac::mean_inference(work, eval::hac::Kernel::BartlettV1, 0U, true);
  if (!iid.defined) return out;
  out.standard_error = std::max(out.standard_error,
      iid.se * static_cast<f64>(daily.size()) / static_cast<f64>(out.valid_dates));
  out.upper_abs_ic = std::abs(out.mean) + cfg.confidence_multiplier * out.standard_error;
  out.suggestive_direction = std::abs(out.mean) >= kDirectionalSe * out.standard_error;
  out.defined = std::isfinite(out.upper_abs_ic) && out.standard_error > 0.0;
  return out;
}

void evaluate_row(std::span<const f64> signal, std::span<const f64> labels,
                  std::span<const f64> cached_ranks, usize cached_names,
                  usize eligible, const IcScreenConfig& cfg, RowScratch& scratch,
                  usize& ranked_count,
                  f64& pearson, f64& rank,atx::u64* paired=nullptr) noexcept {
  pearson = kNan; rank = kNan;
  const bool cached_admitted=cached_names >= cfg.min_names &&
      static_cast<f64>(cached_names) >= kMinCoverage * static_cast<f64>(eligible);
  if (!cached_admitted && paired==nullptr) return; // exact old fast refusal path
  usize count = 0;
  bool same_names = true;
  for (usize i = 0; i < signal.size(); ++i) {
    if (std::isfinite(signal[i]) && std::isfinite(labels[i])) {
      same_names = same_names && count < ranked_count && scratch.ranked_names[count] == i;
      scratch.ranked_names[count] = i;
      scratch.x[count] = signal[i]; scratch.y[count] = labels[i];
      scratch.yr[count] = cached_ranks[i]; ++count;
    }
  }
  if (paired!=nullptr) *paired+=static_cast<atx::u64>(count);
  if (!cached_admitted || count < cfg.min_names ||
      static_cast<f64>(count) < kMinCoverage * static_cast<f64>(cached_names) ||
      static_cast<f64>(count) < kMinCoverage * static_cast<f64>(eligible)) {
    ranked_count = 0U;
    return;
  }
  const auto x = std::span{scratch.x}.first(count);
  const auto y = std::span{scratch.y}.first(count);
  auto xr = std::span{scratch.xr}.first(count);
  auto yr = std::span{scratch.yr}.first(count);
  pearson = correlation(x, y);
  // The date-outer caller keeps signal values fixed. Reuse exact tied ranks
  // only when the ordered paired instrument IDs also match. Equal counts
  // alone are insufficient when missing labels differ across horizons.
  if (!same_names || count != ranked_count) rank_values(x, xr, scratch.order);
  ranked_count = count;
  if (count != cached_names) {
    // Cached ranks preserve ordering/ties but their gaps are invalid on a
    // candidate-specific subset. Re-rank that subset for exact Spearman.
    std::copy(yr.begin(), yr.end(), scratch.y.begin());
    rank_values(std::span{scratch.y}.first(count), yr, scratch.order);
  }
  rank = correlation(xr, yr);
}
} // namespace

namespace {
atx::core::Result<std::shared_ptr<const Cache>> prepare_cache(
    const alpha::Panel& panel, const IcScreenConfig& config, const ResearchIcOptions& options,
    std::span<const atx::u8> member, std::span<const atx::u32> bad,
    std::string_view price_field) {
  static_assert(max_research_ic_workers == 16, "keep the refusal text in step with the bound");
  if (options.workers == 0 || options.workers > max_research_ic_workers)
    return Err(ErrorCode::InvalidArgument, "IC research: workers must be explicit 1..16");
  const auto valid = validate(panel, config, member, bad,options.active_horizons);
  if (!valid) return Err(valid.error());
  auto data = std::make_shared<Cache>();
  data->config = config; data->dates = panel.dates(); data->instruments = panel.instruments();
  data->options=options;
  if (config.rule == IcScreenRule::DisabledV1) return Ok(std::shared_ptr<const Cache>{std::move(data)});
  auto& cfg = data->config;
  if (cfg.window_end == 0U) cfg.window_end = panel.dates();
  if (cfg.maturity_end == 0U) cfg.maturity_end = cfg.window_end;
  data->rows = cfg.window_end - cfg.window_begin;
  const usize mature = std::min(cfg.window_end, cfg.maturity_end);
  const usize available = mature > cfg.window_begin ? mature - cfg.window_begin : 0U;
  const usize n = panel.instruments();
  atx::u64 bytes = sizeof(Cache);
  if (!checked_add_bytes(bytes, data->rows, sizeof(usize)))
    return Err(ErrorCode::InvalidArgument, "IC screen: cache size overflow");
  for (usize k = 0; k < options.active_horizons; ++k) {
    const usize lag = cfg.execution_delay + cfg.horizons[k];
    data->active[k] = available > lag ? available - lag : 0U;
    data->coverage[k].mature_dates=data->active[k];
    data->coverage[k].structural_tail_dates=data->rows-data->active[k];
    const usize cells = data->active[k] * n; // <= validated panel geometry
    if (!checked_add_bytes(bytes, cells, 2U * sizeof(f64)) ||
        !checked_add_bytes(bytes, data->active[k], sizeof(usize)))
      return Err(ErrorCode::InvalidArgument, "IC screen: cache size overflow");
  }
  data->bytes = bytes;
  if (!checked_add_bytes(bytes, n, sizeof(usize)) || bytes > cfg.max_cache_bytes)
    return Err(ErrorCode::InvalidArgument, "IC screen: cache memory budget exceeded");
  ATX_TRY(const auto fid, panel.field_id(price_field));
  const auto prices = panel.field_all(fid);
  if (prices.size() != panel.dates() * n)
    return Err(ErrorCode::InvalidArgument, "IC screen: price shape mismatch");
  std::vector<usize> order(n);
  data->eligible.assign(data->rows, 0U);
  for (usize row = 0; row < data->active.front(); ++row) {
    const usize d = cfg.window_begin + row;
    for (usize i = 0; i < n; ++i)
      if (panel.in_universe(d, i) && (member.empty() || member[d * n + i] != 0U)) ++data->eligible[row];
  }
  for (usize k = 0; k < options.active_horizons; ++k) {
    const usize cells = data->active[k] * n;
    data->labels[k].assign(cells, kNan); data->ranks[k].assign(cells, kNan);
    data->names[k].assign(data->active[k], 0U);
    for (usize row = 0; row < data->active[k]; ++row) {
      const usize d = cfg.window_begin + row, entry = d + cfg.execution_delay;
      const usize end = entry + cfg.horizons[k];
      auto labels = std::span{data->labels[k]}.subspan(row * n, n);
      for (usize i = 0; i < n; ++i) {
        if (!panel.in_universe(d, i) || (!member.empty() && member[d * n + i] == 0U)) continue;
        auto& coverage=data->coverage[k]; ++coverage.decision_eligible_pairs;
        const bool missing_entry=options.require_endpoint_presence && !panel.in_universe(entry,i);
        const bool missing_exit=options.require_endpoint_presence && !panel.in_universe(end,i);
        coverage.missing_entry_pairs+=missing_entry; coverage.missing_exit_pairs+=missing_exit;
        if (missing_entry || missing_exit) continue;
        if (!bad.empty() && bad[end*n+i]!=bad[entry*n+i]) { ++coverage.guard_excluded_pairs; continue; }
        const f64 decision = prices[d * n + i];
        const f64 from = prices[entry * n + i], to = prices[end * n + i];
        if (!std::isfinite(decision) || decision <= 0.0 || !std::isfinite(from) ||
            !std::isfinite(to) || from <= 0.0 || to <= 0.0) { ++coverage.invalid_price_pairs; continue; }
        const f64 ret = to / from - 1.0;
        if (!std::isfinite(ret)) { ++coverage.nonfinite_return_pairs; continue; }
        labels[i] = ret; ++data->names[k][row]; ++coverage.finite_label_pairs;
      }
      rank_values(labels, std::span{data->ranks[k]}.subspan(row * n, n), order);
    }
  }
  return Ok(std::shared_ptr<const Cache>{std::move(data)});
}
} // namespace
atx::core::Result<IcScreenCache> prepare_ic_screen(
    const alpha::Panel& panel,const IcScreenConfig& config,std::span<const atx::u8> member,
    std::span<const atx::u32> bad,std::string_view price_field) {
  IcScreenCache out;
  ATX_TRY(out.data_,prepare_cache(panel,config,{4,false},member,bad,price_field));
  return Ok(std::move(out));
}
atx::core::Result<ResearchIcCache> prepare_research_ic(
    const alpha::Panel& panel,const IcScreenConfig& config,const ResearchIcOptions& options,
    std::span<const atx::u8> member,std::span<const atx::u32> bad,std::string_view price_field) {
  ResearchIcCache out;
  ATX_TRY(out.data_,prepare_cache(panel,config,options,member,bad,price_field));
  return Ok(std::move(out));
}

namespace {
atx::core::Result<std::unique_ptr<Scratch>> prepare_scratch(std::shared_ptr<const Cache> cache) {
  if (!cache) return Err(ErrorCode::InvalidArgument, "IC screen: unprepared cache");
  atx::u64 bytes = sizeof(Scratch);
  if (cache->config.rule != IcScreenRule::DisabledV1) {
    if (!checked_add_bytes(bytes, cache->options.workers, sizeof(RowScratch)) ||
        !checked_add_bytes(bytes, cache->instruments,
            cache->options.workers * (4U * sizeof(f64) + 2U * sizeof(usize))) ||
        !checked_add_bytes(bytes, cache->rows, sizeof(f64)))
      return Err(ErrorCode::InvalidArgument, "IC screen: scratch size overflow");
    for (const usize rows : cache->active)
      if (!checked_add_bytes(bytes, rows, 2U * sizeof(f64)))
        return Err(ErrorCode::InvalidArgument, "IC screen: scratch size overflow");
    // One-worker legacy retains its separate scratch cap. Parallel research
    // must fit its aggregate cache + all row workspaces under the supplied cap.
    if (bytes > cache->config.max_cache_bytes ||
        (cache->options.workers > 1 && cache->bytes > cache->config.max_cache_bytes - bytes))
      return Err(ErrorCode::InvalidArgument, "IC screen: worker scratch memory budget exceeded");
  }
  auto result = std::make_unique<Scratch>();
  auto& s = *result;
  s.bytes=bytes;
  s.cache = cache;
  if (s.cache->config.rule == IcScreenRule::DisabledV1) return Ok(std::move(result));
  const usize n = s.cache->instruments;
  s.rows.resize(s.cache->options.workers);
  for (auto& row : s.rows) {
    row.x.resize(n); row.y.resize(n); row.xr.resize(n); row.yr.resize(n);
    row.order.resize(n); row.ranked_names.resize(n);
  }
  s.influence.resize(s.cache->rows);
  for (usize k = 0; k < s.pearson.size(); ++k) {
    s.pearson[k].resize(s.cache->active[k]); s.rank[k].resize(s.cache->active[k]);
  }
  return Ok(std::move(result));
}

} // namespace
atx::core::Result<IcScreenScratch> prepare_ic_screen_scratch(const IcScreenCache& cache) {
  IcScreenScratch out; ATX_TRY(out.data_,prepare_scratch(cache.data_)); return Ok(std::move(out));
}
atx::core::Result<ResearchIcScratch> prepare_research_ic_scratch(const ResearchIcCache& cache) {
  ResearchIcScratch out; ATX_TRY(out.data_,prepare_scratch(cache.data_)); return Ok(std::move(out));
}

bool ic_screen_cache_matches(const IcScreenCache& cache, const alpha::Panel& panel,
                              const IcScreenConfig& config) noexcept {
  if (!cache.data_ || cache.data_->dates != panel.dates() ||
      cache.data_->instruments != panel.instruments()) return false;
  const auto& prepared = cache.data_->config;
  if (prepared.rule != config.rule) return false;
  if (config.rule == IcScreenRule::DisabledV1) return true;
  const usize end = config.window_end == 0U ? panel.dates() : config.window_end;
  const usize maturity = config.maturity_end == 0U ? end : config.maturity_end;
  return prepared.horizons == config.horizons &&
         prepared.execution_delay == config.execution_delay &&
         prepared.window_begin == config.window_begin && prepared.window_end == end &&
         prepared.maturity_end == maturity && prepared.min_names == config.min_names &&
         prepared.min_dates == config.min_dates && prepared.practical_abs_ic == config.practical_abs_ic &&
         prepared.confidence_multiplier == config.confidence_multiplier &&
         prepared.max_cache_bytes == config.max_cache_bytes;
}

namespace {
IcScreenResult classify_estimates(const std::array<IcScreenHorizon,4>&,
                                  const IcScreenConfig&,usize) noexcept;
atx::core::Result<IcScreenResult> evaluate_cache(std::span<const f64> signal,
    const std::shared_ptr<const Cache>& cache,Scratch& scratch,
    std::array<ResearchIcCoverage,4>* coverage=nullptr,parallel::DetPool* pool=nullptr) {
  const auto& data = *cache;
  const auto& cfg = data.config;
  IcScreenResult out;
  if (cfg.rule == IcScreenRule::DisabledV1) return Ok(out);
  if (signal.size() != data.dates * data.instruments)
    return Err(ErrorCode::InvalidArgument, "IC screen: signal shape mismatch");
  auto& s = scratch;
  for (auto& row : s.rows) row.paired.fill(0);
  const auto run_rows = [&](usize begin, usize end, usize worker) {
    auto& work = s.rows[worker];
    for (usize row = begin; row < end; ++row) {
      usize ranked_count = 0U;
      for (usize k = 0; k < data.options.active_horizons; ++k) {
        if (row >= data.active[k]) continue;
        const usize n = data.instruments, d = cfg.window_begin + row;
        evaluate_row(signal.subspan(d * n, n), std::span{data.labels[k]}.subspan(row * n, n),
                     std::span{data.ranks[k]}.subspan(row * n, n), data.names[k][row],
                     data.eligible[row], cfg, work, ranked_count, s.pearson[k][row], s.rank[k][row],
                     coverage?&work.paired[k]:nullptr);
      }
    }
  };
  if (pool != nullptr && data.active.front() > 1) {
    const usize count = data.active.front();
    const usize bands = std::min(count, data.options.workers * 4U);
    pool->parallel_for(bands, [&](usize band, usize worker) {
      // Quotient/remainder split avoids overflow from count*band.
      const usize begin = count / bands * band + count % bands * band / bands;
      const usize end = count / bands * (band + 1U) + count % bands * (band + 1U) / bands;
      run_rows(begin, end, worker);
    });
  } else {
    run_rows(0, data.active.front(), 0);
  }
  if (coverage != nullptr) for (usize worker = 0; worker < s.rows.size(); ++worker)
    for (usize k = 0; k < data.options.active_horizons; ++k)
      (*coverage)[k].paired_signal_pairs += s.rows[worker].paired[k];
  // Only the row kernels are parallel: retain the original chronological
  // mean, segment and HAC arithmetic, shared influence buffer and horizon order.
  for (usize k = 0; k < data.options.active_horizons; ++k) {
    auto& h = out.horizons[k]; h.horizon = cfg.horizons[k];
    h.pearson = estimate(s.pearson[k], h.horizon, cfg, s.influence);
    h.rank = estimate(s.rank[k], h.horizon, cfg, s.influence);
  }
  return Ok(classify_estimates(out.horizons,cfg,data.options.active_horizons));
}

} // namespace
atx::core::Result<IcScreenResult> screen_ic(std::span<const f64> signal,
    const IcScreenCache& cache,IcScreenScratch& scratch) {
  if (!cache.data_ || !scratch.data_ || scratch.data_->cache != cache.data_)
    return Err(ErrorCode::InvalidArgument,"IC screen: scratch/cache binding mismatch");
  return evaluate_cache(signal,cache.data_,*scratch.data_);
}
atx::core::Result<ResearchIcResult> evaluate_research_ic(std::span<const f64> signal,
    const ResearchIcCache& cache,ResearchIcScratch& scratch,parallel::DetPool* pool) {
  if (!cache.data_ || !scratch.data_ || scratch.data_->cache != cache.data_)
    return Err(ErrorCode::InvalidArgument,"IC research: scratch/cache binding mismatch");
  const auto workers = cache.data_->options.workers;
  if ((workers == 1 && pool != nullptr) ||
      (workers > 1 && (pool == nullptr || pool->n_workers() != workers)))
    return Err(ErrorCode::InvalidArgument,"IC research: borrowed pool/worker recipe mismatch");
  ResearchIcResult out; out.active_horizons=cache.data_->options.active_horizons;
  out.coverage=cache.data_->coverage;
  ATX_TRY(out.screen,evaluate_cache(signal,cache.data_,*scratch.data_,&out.coverage,pool));
  return Ok(std::move(out));
}

IcScreenConfig equivalence_ic_screen_config() noexcept {
  IcScreenConfig cfg;
  cfg.rule = IcScreenRule::EquivalenceV3;
  cfg.practical_abs_ic = 0.002;
  cfg.confidence_multiplier = 3.5;
  return cfg;
}
IcScreenConfig research_window_ic_config(usize begin,usize end,usize min_names,usize min_dates,
                                         atx::u64 max_cache_bytes) noexcept {
  auto ic=equivalence_ic_screen_config();
  ic.horizons={5,21,63,0}; ic.min_names=min_names; ic.min_dates=min_dates;
  ic.window_begin=begin; ic.window_end=end; ic.maturity_end=end;
  ic.max_cache_bytes=max_cache_bytes;
  return ic;
}

namespace {
IcScreenResult classify_estimates(const std::array<IcScreenHorizon, 4>& estimates,
                                            const IcScreenConfig& cfg,usize active_horizons) noexcept {
  IcScreenResult out;
  if (cfg.rule == IcScreenRule::DisabledV1) return out;
  out.horizons = estimates;
  out.reason = IcScreenReason::InsufficientEvidence;
  if (cfg.rule != IcScreenRule::ConservativeV2 && cfg.rule != IcScreenRule::EquivalenceV3)
    return out;
  const bool equivalence = cfg.rule == IcScreenRule::EquivalenceV3;
  if (equivalence && (!std::isfinite(cfg.practical_abs_ic) || cfg.practical_abs_ic <= 0.0 ||
      cfg.practical_abs_ic >= 1.0 || !std::isfinite(cfg.confidence_multiplier) ||
      cfg.confidence_multiplier < 3.0)) return out;
  out.enough_evidence = true;
  bool possible = false;
  for (usize k=0;k<active_horizons;++k) {
    auto& h=out.horizons[k];
    for (auto* e : {&h.pearson, &h.rank}) {
      if (equivalence) {
        e->upper_abs_ic = std::abs(e->mean) + cfg.confidence_multiplier * e->standard_error;
        e->defined = e->defined && std::isfinite(e->mean) && std::isfinite(e->standard_error) &&
                     e->standard_error > 0.0 && std::isfinite(e->upper_abs_ic) &&
                     std::isfinite(e->max_segment_abs_ic) && e->max_segment_abs_ic >= 0.0;
      }
      possible = possible || (!equivalence && e->suggestive_direction) ||
                 e->upper_abs_ic >= cfg.practical_abs_ic ||
                 e->max_segment_abs_ic >= cfg.practical_abs_ic;
    }
    h.enough_evidence = h.pearson.defined && h.rank.defined;
    out.enough_evidence = out.enough_evidence && h.enough_evidence;
  }
  out.reject = out.enough_evidence && !possible;
  out.reason = !out.enough_evidence ? IcScreenReason::InsufficientEvidence :
      (out.reject ? IcScreenReason::PracticalNull : IcScreenReason::PossibleAlpha);
  return out;
}

} // namespace
IcScreenResult classify_ic_screen_estimates(const std::array<IcScreenHorizon,4>& estimates,
    const IcScreenConfig& cfg) noexcept { return classify_estimates(estimates,cfg,4); }

usize IcScreenCache::dates() const noexcept { return data_ ? data_->dates : 0U; }
usize IcScreenCache::instruments() const noexcept { return data_ ? data_->instruments : 0U; }
usize IcScreenCache::first_date() const noexcept { return data_ ? data_->config.window_begin : 0U; }
usize IcScreenCache::cached_dates() const noexcept { return data_ ? data_->rows : 0U; }
atx::u64 IcScreenCache::bytes() const noexcept { return data_ ? data_->bytes : 0U; }
std::span<const f64> IcScreenCache::returns(usize k) const noexcept {
  return data_ && k < 4U ? std::span<const f64>{data_->labels[k]} : std::span<const f64>{};
}
std::span<const f64> IcScreenCache::return_ranks(usize k) const noexcept {
  return data_ && k < 4U ? std::span<const f64>{data_->ranks[k]} : std::span<const f64>{};
}
IcScreenScratch::IcScreenScratch() = default;
IcScreenScratch::~IcScreenScratch() = default;
IcScreenScratch::IcScreenScratch(IcScreenScratch&&) noexcept = default;
IcScreenScratch& IcScreenScratch::operator=(IcScreenScratch&&) noexcept = default;
std::span<const f64> IcScreenScratch::pearson_series(usize k) const noexcept {
  return data_ && k < 4U ? std::span<const f64>{data_->pearson[k]} : std::span<const f64>{};
}
std::span<const f64> IcScreenScratch::rank_series(usize k) const noexcept {
  return data_ && k < 4U ? std::span<const f64>{data_->rank[k]} : std::span<const f64>{};
}
usize ResearchIcCache::dates() const noexcept { return data_?data_->dates:0; }
usize ResearchIcCache::instruments() const noexcept { return data_?data_->instruments:0; }
usize ResearchIcCache::first_date() const noexcept { return data_?data_->config.window_begin:0; }
atx::u64 ResearchIcCache::bytes() const noexcept { return data_?data_->bytes:0; }
std::span<const f64> ResearchIcCache::labels(usize k) const noexcept {
  return data_ && k<data_->options.active_horizons?std::span<const f64>{data_->labels[k]}:std::span<const f64>{};
}
usize ResearchIcCache::label_rows(usize k) const noexcept {
  return data_ && k<data_->options.active_horizons?data_->active[k]:0;
}
ResearchIcScratch::ResearchIcScratch()=default;
ResearchIcScratch::~ResearchIcScratch()=default;
ResearchIcScratch::ResearchIcScratch(ResearchIcScratch&&) noexcept=default;
ResearchIcScratch& ResearchIcScratch::operator=(ResearchIcScratch&&) noexcept=default;
atx::u64 ResearchIcScratch::bytes() const noexcept { return data_?data_->bytes:0; }
std::span<const f64> ResearchIcScratch::pearson_series(usize k) const noexcept {
  return data_ && k<data_->cache->options.active_horizons?std::span<const f64>{data_->pearson[k]}:std::span<const f64>{};
}
std::span<const f64> ResearchIcScratch::rank_series(usize k) const noexcept {
  return data_ && k<data_->cache->options.active_horizons?std::span<const f64>{data_->rank[k]}:std::span<const f64>{};
}
std::string_view ic_screen_reason_name(IcScreenReason reason) noexcept {
  switch (reason) {
  case IcScreenReason::Disabled: return "disabled";
  case IcScreenReason::InsufficientEvidence: return "insufficient-evidence";
  case IcScreenReason::PossibleAlpha: return "possible-alpha";
  case IcScreenReason::PracticalNull: return "practical-null";
  }
  return "unknown";
}
std::string_view ic_screen_rule_name(IcScreenRule rule) noexcept {
  switch (rule) {
  case IcScreenRule::DisabledV1: return "disabled-v1";
  case IcScreenRule::ConservativeV2: return "conservative-v2";
  case IcScreenRule::EquivalenceV3: return "equivalence-v3";
  }
  return "unknown";
}
usize ic_screen_simd_width() noexcept { return Batch::size; }
} // namespace atx::engine::factory
