#include "strategy_ic_composition.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace atx::impl::strategy {
namespace {
using namespace atx;
namespace co = atx::core;
constexpr usize max_candidates = 256;
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
} // namespace

co::Result<u64> ic_composition_working_bytes(usize dates, usize names, usize count) {
  if (!dates || !names || !count || count > max_candidates)
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
  return co::Ok(total);
}

struct IcComposition::Impl {
  IcCompositionConfig cfg;
  std::vector<IcCompositionCandidate> candidates;
  std::vector<f64> weights;
  std::vector<u8> member;
  std::vector<Ranked> row;
  std::vector<f64> current, target;
  IcCompositionResult result;
  usize next{};
  bool finished{};
};
IcComposition::IcComposition(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
IcComposition::~IcComposition() = default;
IcComposition::IcComposition(IcComposition&&) noexcept = default;
IcComposition& IcComposition::operator=(IcComposition&&) noexcept = default;

co::Result<IcComposition> IcComposition::create(const IcCompositionConfig& cfg,
    std::span<const IcCompositionCandidate> candidates, std::span<const u8> member) {
  ATX_TRY(auto bytes, ic_composition_working_bytes(cfg.dates, cfg.instruments, candidates.size()));
  if (bytes > cfg.max_working_bytes)
    return co::Err(co::ErrorCode::OutOfRange, "IC composition: working budget exceeded");
  const usize cells = cfg.dates * cfg.instruments;
  if (member.size() != cells || cfg.decision_begin >= cfg.decision_end ||
      cfg.decision_end > cfg.dates || !cfg.cadence ||
      !std::isfinite(cfg.trade_fraction) || cfg.trade_fraction <= 0 || cfg.trade_fraction > 1 ||
      std::any_of(member.begin(), member.end(), [](u8 v) { return v > 1; }))
    return co::Err(co::ErrorCode::InvalidArgument, "IC composition: config/membership mismatch");
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
      const auto n = std::count_if(candidates.begin(), candidates.end(), [&](const auto& c) {
        return c.family == candidates[i].family;
      });
      p->weights[i] = 1.0 / (static_cast<f64>(families) * static_cast<f64>(n));
    }
    p->row.reserve(cfg.instruments); p->current.resize(cfg.instruments); p->target.resize(cfg.instruments);
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

co::Status IcComposition::add(usize index, std::span<const f64> signal, int sign) {
  if (!impl_ || impl_->finished || index != impl_->next || index >= impl_->candidates.size() ||
      sign < -1 || sign > 1)
    return co::Err(co::ErrorCode::InvalidArgument, "IC composition: candidate order/finished state");
  auto& p = *impl_;
  if (signal.size() != p.result.signal.size() && !(sign == 0 && signal.empty()))
    return co::Err(co::ErrorCode::InvalidArgument, "IC composition: signal shape mismatch");
  if (sign != 0) for (usize d = 0; d < p.cfg.dates; ++d) {
    p.row.clear(); const auto offset = d * p.cfg.instruments;
    for (usize i = 0; i < p.cfg.instruments; ++i)
      if (p.member[offset + i] && std::isfinite(signal[offset + i]))
        p.row.emplace_back(signal[offset + i], i);
    if (p.row.size() < 2) continue;
    sort_ranks(p.row);
    each_centered_rank(p.row, [&](usize i, f64 r) {
      p.result.signal[offset + i] += static_cast<f64>(sign) * p.weights[index] * r;
    });
    p.result.contribution_fraction[d] += p.weights[index] * static_cast<f64>(p.row.size());
  }
  ++p.next; return co::Ok();
}

co::Result<IcCompositionResult> IcComposition::finish() {
  if (!impl_ || impl_->finished || impl_->next != impl_->candidates.size())
    return co::Err(co::ErrorCode::InvalidArgument, "IC composition: incomplete/finished input");
  auto& p = *impl_; auto& out = p.result;
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
