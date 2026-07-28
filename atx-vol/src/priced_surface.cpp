#include "atx/vol/priced_surface.hpp"

#include "atx/vol/american_batch.hpp" // exact resolved price-only batch
#include "atx/vol/correction.hpp"
#include "atx/vol/counters.hpp"
#include "atx/vol/pricing_executor.hpp" // the ONE bounded pricing pool (accelerator build)
#include "laned_greek_run.hpp" // WS-P1v: the shared laned analytic-Greek batch driver
#include "term_carry.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// A discount-factor difference at this scale changes log-forward by at most
// approximately 1e-12. Treating it as the configured scalar rate is therefore
// economically immaterial while absorbing serialization/libm roundoff.
constexpr double kFlatDiscountRelativeTolerance = 1.0e-12;

[[nodiscard]] bool discount_matches_scalar_rate(double df, double T, double rate) noexcept {
  if (!(df > 0.0) || !std::isfinite(df) || !(T > 0.0) || !std::isfinite(T)) {
    return false;
  }
  const double expected = std::exp(-rate * T);
  if (!(expected > 0.0) || !std::isfinite(expected)) {
    return false;
  }
  const double scale = std::max(std::abs(df), std::abs(expected));
  return std::abs(df - expected) <= kFlatDiscountRelativeTolerance * scale;
}

[[nodiscard]] std::uint64_t allocate_surface_instance_id() noexcept {
  static std::atomic<std::uint64_t> next{1};
  std::uint64_t candidate = next.load();
  for (;;) {
    if (candidate == std::numeric_limits<std::uint64_t>::max()) {
      std::terminate();
    }
    if (next.compare_exchange_weak(candidate, candidate + 1)) {
      return candidate;
    }
  }
}

void poison(AmericanGreeks &greeks) noexcept {
  greeks.delta = greeks.gamma = greeks.vega = greeks.theta = greeks.rho = greeks.vanna =
      greeks.volga = greeks.charm = greeks.price = kNaN;
}

[[nodiscard]] bool valid_query(double K, double T) noexcept {
  return std::isfinite(K) && (K > 0.0) && std::isfinite(T) && (T > 0.0);
}

template <class Input, class Output>
[[nodiscard]] bool spans_overlap(std::span<Input> input, std::span<Output> output) noexcept {
  if (input.empty() || output.empty()) {
    return false;
  }
  // SAFETY: converting valid object pointers to uintptr_t is implementation-
  // defined but supported by both repository toolchains. Integer comparison
  // avoids undefined relational comparison between unrelated object pointers.
  const std::uintptr_t input_begin = reinterpret_cast<std::uintptr_t>(input.data());
  const std::uintptr_t output_begin = reinterpret_cast<std::uintptr_t>(output.data());
  if (input_begin <= output_begin) {
    return (output_begin - input_begin) < input.size_bytes();
  }
  return (input_begin - output_begin) < output.size_bytes();
}

} // namespace

FullGreekSeed::FullGreekSeed(std::uint32_t uid, double K, double T, Side side,
                             std::uint64_t surface_instance_id, bool analytic_greeks,
                             QueryExecution query_execution, double iv,
                             const AmericanGreeks &greeks) noexcept
    : uid_(uid), K_(K), T_(T), side_(side), surface_instance_id_(surface_instance_id),
      analytic_greeks_(analytic_greeks), query_execution_(query_execution), iv_(iv),
      greeks_(greeks) {}

struct PricedSurface::QueryAccelerator {
  struct Entry {
    double T{0.0};
    double rate{0.0};
    double q_eff{0.0};
    std::optional<CorrectionCache> call{};
    std::optional<CorrectionCache> put{};
  };

  struct Domain {
    double k_min{-0.75};
    double k_max{0.75};
    double T_min{0.0};
    double T_max{0.0};
    double sigma_min{0.05};
    double sigma_max{1.50};
  };

  [[nodiscard]] static Domain domain_from(const PricedSurface &surface) noexcept {
    Domain domain;
    double node_k_min = std::numeric_limits<double>::infinity();
    double node_k_max = -std::numeric_limits<double>::infinity();
    for (const std::unique_ptr<IVolCurve> &curve : surface.surface_.slices()) {
      if (curve->kind() == VolCurveKind::LinearVariance) {
        const auto *linear = dynamic_cast<const LinearVarianceCurve *>(curve.get());
        if (linear != nullptr) {
          for (const double k : linear->k_nodes()) {
            if (std::isfinite(k)) {
              node_k_min = std::min(node_k_min, k);
              node_k_max = std::max(node_k_max, k);
            }
          }
        }
      } else if (curve->kind() == VolCurveKind::ConvexDense) {
        const auto *convex = dynamic_cast<const ConvexDenseCurve *>(curve.get());
        if (convex != nullptr && convex->fit().F > 0.0) {
          for (const double strike : convex->fit().u) {
            if (strike > 0.0 && std::isfinite(strike)) {
              const double k = std::log(strike / convex->fit().F);
              node_k_min = std::min(node_k_min, k);
              node_k_max = std::max(node_k_max, k);
            }
          }
        }
      }
    }
    if (std::isfinite(node_k_min) && std::isfinite(node_k_max) && node_k_max > node_k_min) {
      domain.k_min = node_k_min - 0.05;
      domain.k_max = node_k_max + 0.05;
    }
    const double first_T = surface.ctx_.front().T;
    const double last_T = surface.ctx_.back().T;
    domain.T_min = 0.9 * first_T;
    domain.T_max = (last_T > first_T) ? 1.1 * last_T : 1.5 * first_T;
    return domain;
  }

  [[nodiscard]] static Result<Entry> build_entry(const PricedSurface &surface, const Domain &domain,
                                                 std::size_t context_index, bool parallel_sides) {
    constexpr std::uint16_t kNK = 16;
    constexpr std::uint16_t kNT = 8;
    constexpr std::uint16_t kNS = 12;
    const SliceContext &context = surface.ctx_[context_index];
    Entry entry;
    entry.T = context.T;
    entry.rate = surface.rate_at(context.T);
    entry.q_eff = context.q_eff;
    const auto build_side = [&](Side side) {
      return CorrectionCache::build(kNK, kNT, kNS, entry.rate, entry.q_eff, domain.k_min,
                                    domain.k_max, domain.T_min, domain.T_max, domain.sigma_min,
                                    domain.sigma_max, side,
                                    std::optional<AlOpts>{surface.pricing_.al_opts});
    };
    // The single-center (RepresentativeFast) build overlaps its two sides. This
    // pair stays OFF the pricing pool deliberately: it is 2 units of work, below
    // the executor's inline threshold, so routing it there would serialize two
    // multi-millisecond cache builds — a latency regression, not a fix. What IS
    // fixed is the escape hatch: thread exhaustion used to leave this
    // Result-returning function as a std::system_error. It now degrades to the
    // serial build, which produces the identical entry.
    std::future<Result<CorrectionCache>> put_future;
    bool put_async = false;
    if (parallel_sides) {
      try {
        put_future = std::async(std::launch::async, build_side, Side::Put);
        put_async = true;
      } catch (const std::system_error &) {
        // No execution agent available; `put_async` stays false and the put side
        // is built inline below.
      }
    }
    auto call = build_side(Side::Call);
    if (!call.has_value()) {
      if (put_async) {
        (void)put_future.get();
      }
      return Err(call.error());
    }
    auto put = put_async ? put_future.get() : build_side(Side::Put);
    if (!put.has_value()) {
      return Err(put.error());
    }
    entry.call = std::move(*call);
    entry.put = std::move(*put);
    return Ok(std::move(entry));
  }

  [[nodiscard]] static std::size_t representative_index(const PricedSurface &surface) noexcept {
    std::size_t best = 0u;
    double best_score = std::numeric_limits<double>::infinity();
    for (std::size_t candidate = 0u; candidate < surface.ctx_.size(); ++candidate) {
      const double candidate_rate = surface.rate_at(surface.ctx_[candidate].T);
      double score = 0.0;
      for (std::size_t peer = 0u; peer < surface.ctx_.size(); ++peer) {
        const double weight =
            static_cast<double>(std::max<std::size_t>(1u, surface.ctx_[peer].n_used));
        score += weight * (std::fabs(candidate_rate - surface.rate_at(surface.ctx_[peer].T)) +
                           std::fabs(surface.ctx_[candidate].q_eff - surface.ctx_[peer].q_eff));
      }
      if (score < best_score) {
        best = candidate;
        best_score = score;
      }
    }
    return best;
  }

  [[nodiscard]] static std::vector<std::size_t> bank_indices(const PricedSurface &surface) {
    constexpr std::size_t kMaxCarryCenters = 16u;
    const std::size_t target = std::min(kMaxCarryCenters, surface.ctx_.size());
    std::vector<std::size_t> selected{0u};
    selected.reserve(target);
    while (selected.size() < target) {
      std::size_t farthest = surface.ctx_.size();
      double farthest_distance = 0.0;
      for (std::size_t candidate = 0u; candidate < surface.ctx_.size(); ++candidate) {
        if (std::find(selected.begin(), selected.end(), candidate) != selected.end()) {
          continue;
        }
        const double candidate_rate = surface.rate_at(surface.ctx_[candidate].T);
        double nearest = std::numeric_limits<double>::infinity();
        for (const std::size_t chosen : selected) {
          const double distance =
              std::fabs(candidate_rate - surface.rate_at(surface.ctx_[chosen].T)) +
              std::fabs(surface.ctx_[candidate].q_eff - surface.ctx_[chosen].q_eff);
          nearest = std::min(nearest, distance);
        }
        if (nearest > farthest_distance) {
          farthest = candidate;
          farthest_distance = nearest;
        }
      }
      if (farthest == surface.ctx_.size() || !(farthest_distance > 0.0)) {
        break;
      }
      selected.push_back(farthest);
    }
    std::sort(selected.begin(), selected.end());
    return selected;
  }

  [[nodiscard]] static Result<std::unique_ptr<QueryAccelerator>> build(const PricedSurface &surface,
                                                                       QueryPricingTier tier) {
    auto accelerator = std::make_unique<QueryAccelerator>();
    const Domain domain = domain_from(surface);
    std::vector<std::size_t> indices;
    if (tier == QueryPricingTier::RepresentativeFast) {
      indices.push_back(representative_index(surface));
    } else {
      indices = bank_indices(surface);
    }
    accelerator->entries.reserve(indices.size());
    if (indices.size() == 1u) {
      auto entry = build_entry(surface, domain, indices.front(), true);
      if (!entry.has_value()) {
        return Err(entry.error());
      }
      accelerator->entries.push_back(std::move(*entry));
    } else {
      // Centers are independent immutable derived state (build_entry is a pure
      // function of surface/domain/index — it touches no warm or thread-local
      // state), so they fan out. They fan out through the ONE process pricing
      // pool, NOT `std::async`: a `std::async(launch::async)` per center spawned
      // up to kMaxCarryCenters execution agents outside the executor's core
      // budget, oversubscribing whatever fit/pricing dispatch enclosed the
      // build, and a thread-exhaustion std::system_error escaped this
      // Result-returning function as an exception instead of an Error.
      //
      // Determinism is structural, exactly as before: each center writes its OWN
      // pre-sized slot (disjoint writes over const reads), and the bank plus the
      // first reported error are read back in index order. The executor's block
      // partition never moves which index lands in which slot, so the result is
      // identical for ANY worker count — including the fully inline path taken
      // when this build is nested inside another dispatch.
      std::vector<Result<Entry>> built(indices.size());
      pricing_executor().run_blocks(indices.size(), /*n_threads=*/0, [&](std::size_t slot) {
        built[slot] = build_entry(surface, domain, indices[slot], false);
      });
      for (Result<Entry> &entry : built) {
        if (!entry.has_value()) {
          return Err(entry.error());
        }
        accelerator->entries.push_back(std::move(*entry));
      }
    }
    if (accelerator->entries.empty()) {
      return Err(ErrorCode::InvalidArgument,
                 "PricedSurface::with_query_pricing: no usable cache centers");
    }
    return Ok(std::move(accelerator));
  }

  [[nodiscard]] const CorrectionCache *cache_for(const Entry &entry, Side side) const noexcept {
    if (side == Side::Call) {
      return entry.call.has_value() ? &*entry.call : nullptr;
    }
    return entry.put.has_value() ? &*entry.put : nullptr;
  }

  [[nodiscard]] CorrectionBlend blend_at(const ResolvedSurfacePoint &point, Side side,
                                         QueryPricingTier tier) const noexcept {
    if (entries.empty()) {
      return {};
    }
    const auto usable_single = [&](const Entry &entry) noexcept {
      const CorrectionCache *cache = cache_for(entry, side);
      return cache != nullptr && cache->contains(point.k_log, point.T, point.sigma)
                 ? CorrectionBlend::single(cache)
                 : CorrectionBlend{};
    };
    if (tier == QueryPricingTier::RepresentativeFast || entries.size() == 1u) {
      return usable_single(entries.front());
    }
    const auto upper = std::lower_bound(
        entries.begin(), entries.end(), point.T,
        [](const Entry &entry, double maturity) noexcept { return entry.T < maturity; });
    if (upper == entries.begin()) {
      return usable_single(*upper);
    }
    if (upper == entries.end()) {
      return usable_single(entries.back());
    }
    if (upper->T == point.T) {
      return usable_single(*upper);
    }

    const Entry &hi = *upper;
    const Entry &lo = *(upper - 1);
    const CorrectionCache *lo_cache = cache_for(lo, side);
    const CorrectionCache *hi_cache = cache_for(hi, side);
    if (lo_cache == nullptr || hi_cache == nullptr ||
        !lo_cache->contains(point.k_log, point.T, point.sigma) ||
        !hi_cache->contains(point.k_log, point.T, point.sigma)) {
      return {};
    }
    const double dr = hi.rate - lo.rate;
    const double dq = hi.q_eff - lo.q_eff;
    const double norm2 = dr * dr + dq * dq;
    if (!(norm2 > 0.0) || !std::isfinite(norm2)) {
      return CorrectionBlend::single(lo_cache);
    }
    const double projection = ((point.rate - lo.rate) * dr + (point.q_eff - lo.q_eff) * dq) / norm2;
    const double upper_weight = std::clamp(projection, 0.0, 1.0);
    if (upper_weight == 0.0) {
      return CorrectionBlend::single(lo_cache);
    }
    if (upper_weight == 1.0) {
      return CorrectionBlend::single(hi_cache);
    }
    return CorrectionBlend{lo_cache, hi_cache, upper_weight};
  }

  std::vector<Entry> entries;
};

PricedSurface::~PricedSurface() = default;
PricedSurface::PricedSurface(PricedSurface &&other) noexcept
    : surface_(std::move(other.surface_)), ctx_(std::move(other.ctx_)), pricing_(other.pricing_),
      slice_rates_(std::move(other.slice_rates_)), term_rates_(other.term_rates_),
      query_pricing_tier_(other.query_pricing_tier_),
      query_accelerator_(std::move(other.query_accelerator_)),
      instance_id_(std::exchange(other.instance_id_, allocate_surface_instance_id())) {}

PricedSurface &PricedSurface::operator=(PricedSurface &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  surface_ = std::move(other.surface_);
  ctx_ = std::move(other.ctx_);
  pricing_ = other.pricing_;
  slice_rates_ = std::move(other.slice_rates_);
  term_rates_ = other.term_rates_;
  query_pricing_tier_ = other.query_pricing_tier_;
  query_accelerator_ = std::move(other.query_accelerator_);
  instance_id_ = std::exchange(other.instance_id_, allocate_surface_instance_id());
  return *this;
}

PricedSurface::PricedSurface(CurveSurface &&surface, std::vector<SliceContext> &&ctx,
                             const PricingContext &pricing, std::vector<double> &&slice_rates,
                             bool term_rates) noexcept
    : surface_{std::move(surface)}, ctx_{std::move(ctx)}, pricing_{pricing},
      slice_rates_{std::move(slice_rates)}, term_rates_{term_rates},
      instance_id_{allocate_surface_instance_id()} {}

Result<PricedSurface> PricedSurface::create(CurveSurface &&surface,
                                            std::vector<SliceContext> context,
                                            const PricingContext &pricing) {
  if (surface.empty()) {
    return Err(ErrorCode::InvalidArgument, "PricedSurface::create: empty surface");
  }
  if (context.size() != surface.n_slices()) {
    return Err(ErrorCode::InvalidArgument, "PricedSurface::create: context length != slice count");
  }
  if (!(pricing.S > 0.0) || !std::isfinite(pricing.r)) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurface::create: non-positive spot or non-finite rate");
  }
  for (std::size_t i = 1; i < context.size(); ++i) {
    if (!(context[i].T > context[i - 1].T)) {
      return Err(ErrorCode::InvalidArgument,
                 "PricedSurface::create: slice T's not strictly ascending");
    }
  }
  for (const SliceContext &slice : context) {
    if (!(slice.T > 0.0) || !std::isfinite(slice.T) || !(slice.forward > 0.0) ||
        !std::isfinite(slice.forward) || !std::isfinite(slice.q_eff)) {
      return Err(ErrorCode::InvalidArgument, "PricedSurface::create: invalid slice carry context");
    }
  }
  std::vector<double> slice_rates;
  slice_rates.reserve(surface.n_slices());
  bool term_rates = false;
  for (const std::unique_ptr<IVolCurve> &slice : surface.slices()) {
    const double T = slice->T();
    const double df = slice->df();
    if (discount_matches_scalar_rate(df, T, pricing.r)) {
      slice_rates.push_back(pricing.r);
      continue;
    }
    term_rates = true;
    const double decoded_rate =
        T > 0.0 && df > 0.0 && std::isfinite(df) ? -std::log(df) / T : pricing.r;
    slice_rates.push_back(decoded_rate);
  }
  return PricedSurface{std::move(surface), std::move(context), pricing, std::move(slice_rates),
                       term_rates};
}

Result<PricedSurface> PricedSurface::with_query_pricing(QueryPricingTier tier) && {
  switch (tier) {
  case QueryPricingTier::LegacyCompatible:
  case QueryPricingTier::ColdReference:
    query_pricing_tier_ = tier;
    query_accelerator_.reset();
    instance_id_ = allocate_surface_instance_id();
    return Ok(std::move(*this));
  case QueryPricingTier::RepresentativeFast:
  case QueryPricingTier::CarryBank:
    break;
  }
  if (pricing_.method != AmericanMethod::AndersenLake) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurface::with_query_pricing: fast tiers require Andersen-Lake");
  }
  auto accelerator = QueryAccelerator::build(*this, tier);
  if (!accelerator.has_value()) {
    return Err(accelerator.error());
  }
  query_accelerator_ = std::move(*accelerator);
  query_pricing_tier_ = tier;
  instance_id_ = allocate_surface_instance_id();
  return Ok(std::move(*this));
}

std::size_t PricedSurface::query_cache_pair_count() const noexcept {
  return query_accelerator_ != nullptr ? query_accelerator_->entries.size() : 0u;
}

QueryPricingRoute PricedSurface::query_pricing_route(double K, double T, Side side,
                                                     QueryExecution execution) const noexcept {
  if (execution == QueryExecution::ColdReference) {
    return QueryPricingRoute::ColdReference;
  }
  if (query_pricing_tier_ == QueryPricingTier::LegacyCompatible ||
      query_pricing_tier_ == QueryPricingTier::ColdReference) {
    return QueryPricingRoute::ColdReference;
  }
  const ResolvedSurfacePoint point = resolve(K, T);
  if (!point.valid || query_accelerator_ == nullptr) {
    return QueryPricingRoute::ColdFallback;
  }
  const CorrectionBlend correction = query_accelerator_->blend_at(point, side, query_pricing_tier_);
  if (!correction.usable(side)) {
    return QueryPricingRoute::ColdFallback;
  }
  return query_pricing_tier_ == QueryPricingTier::RepresentativeFast
             ? QueryPricingRoute::RepresentativeFast
             : QueryPricingRoute::CarryBank;
}

PricedSurface::ForwardCarry PricedSurface::interp_forward(double T) const noexcept {
  // Precondition: ctx_ non-empty and ascending in T (create guarantees it). This
  // shares VolaSession::interp_forward's log-forward/discount-state semantics.
  const SliceContext &first = ctx_.front();
  const SliceContext &last = ctx_.back();
  const auto slice_rate = [this](std::size_t index) noexcept {
    return term_rates_ ? slice_rates_[index] : pricing_.r;
  };
  if (T <= first.T) {
    const double rate = slice_rate(0u);
    if (T == first.T) {
      return ForwardCarry{first.forward, first.q_eff, rate};
    }
    const double forward = pricing_.S * std::exp((rate - first.q_eff) * T);
    return ForwardCarry{forward, first.q_eff, rate};
  }
  if (T >= last.T) {
    const double rate = slice_rate(ctx_.size() - 1u);
    if (T == last.T) {
      return ForwardCarry{last.forward, last.q_eff, rate};
    }
    const double forward = pricing_.S * std::exp((rate - last.q_eff) * T);
    return ForwardCarry{forward, last.q_eff, rate};
  }
  // Interior (first.T < T < last.T): `hi` is the first index with ctx_[hi].T > T
  // — exactly where the old linear scan `while (ctx_[hi].T <= T) ++hi` stops. On
  // an exact node hit (T == ctx_[j].T) the `<=` scan steps PAST node j, and
  // upper_bound (first element strictly greater than T) lands on j+1 identically,
  // so lo=j, alpha=0 — the same off-by-one. ctx_ is strictly ascending, so this
  // is bit-for-bit the same (lo, hi) bracket the scan selected.
  const auto it =
      std::upper_bound(ctx_.begin(), ctx_.end(), T,
                       [](double t, const SliceContext &s) noexcept { return t < s.T; });
  const std::size_t hi = static_cast<std::size_t>(it - ctx_.begin());
  const std::size_t lo = hi - 1;
  const SliceContext &a = ctx_[lo];
  const SliceContext &b = ctx_[hi];
  if (T == a.T) {
    return ForwardCarry{a.forward, a.q_eff, slice_rate(lo)};
  }
  const double span = b.T - a.T;
  const double alpha = (span > 0.0) ? (T - a.T) / span : 0.0;
  const double rate_lo = slice_rate(lo);
  const double rate_hi = slice_rate(hi);
  const double forward = interpolate_positive_log(a.forward, b.forward, alpha);
  double rate = pricing_.r;
  if (term_rates_) {
    const double log_df_lo = -rate_lo * a.T;
    const double log_df_hi = -rate_hi * b.T;
    rate = -(log_df_lo + alpha * (log_df_hi - log_df_lo)) / T;
  }
  const double q_eff = coherent_q_eff(pricing_.S, forward, T, rate);
  return ForwardCarry{forward, q_eff, rate};
}

double PricedSurface::forward_at(double T) const noexcept {
  if (!(T > 0.0) || !std::isfinite(T) || ctx_.empty()) {
    return 0.0;
  }
  return interp_forward(T).forward;
}

double PricedSurface::q_eff_at(double T) const noexcept {
  if (!(T > 0.0) || !std::isfinite(T) || ctx_.empty()) {
    return 0.0;
  }
  return interp_forward(T).q_eff;
}

double PricedSurface::rate_at(double T) const noexcept {
  if (!(T > 0.0) || !std::isfinite(T) || ctx_.empty()) {
    return 0.0;
  }
  return interp_forward(T).rate;
}

PricedSurface::ResolvedSurfacePoint PricedSurface::resolve(double K, double T) const noexcept {
  ResolvedSurfacePoint p;
  p.K = K;
  p.T = T;
  if (!valid_query(K, T)) {
    return p; // valid == false; all numeric fields left 0
  }
  return resolve_with_carry(K, T, interp_forward(T));
}

PricedSurface::ResolvedSurfacePoint
PricedSurface::resolve_with_carry(double K, double T, ForwardCarry fc) const noexcept {
  return resolve_with_carry_and_bracket(K, T, fc, surface_.bracket(T));
}

PricedSurface::ResolvedSurfacePoint
PricedSurface::resolve_with_carry_and_bracket(double K, double T, ForwardCarry fc,
                                              CurveSurface::Bracket bracket) const noexcept {
  // Precondition: T is a valid query T (finite, > 0) so `fc` == interp_forward(T);
  // only K's validity is re-checked here so a strike ladder can reuse one carry
  // and one surface bracket.
  ResolvedSurfacePoint p;
  p.K = K;
  p.T = T;
  if (!(std::isfinite(K) && (K > 0.0))) {
    return p; // valid == false
  }
  p.forward = fc.forward;
  p.q_eff = fc.q_eff;
  p.rate = fc.rate;
  p.k_log = std::log(K / fc.forward);
  p.sigma = surface_.iv(p.k_log, T, bracket);
  p.valid = true;
  return p;
}

double PricedSurface::iv(double K, double T) const noexcept {
  const ResolvedSurfacePoint p = resolve(K, T);
  return p.valid ? p.sigma : kNaN;
}

double PricedSurface::total_variance(double K, double T) const noexcept {
  const ResolvedSurfacePoint p = resolve(K, T);
  // Same k_log as iv(); total variance is the curve's w(k, T) (not sigma).
  return p.valid ? surface_.w(p.k_log, T) : kNaN;
}

Result<double> PricedSurface::price_resolved(const ResolvedSurfacePoint &p, Side side,
                                             QueryExecution execution) const {
  counters::lightweight::QuerySample telemetry_sample{execution == QueryExecution::Configured &&
                                                      query_accelerator_ != nullptr};
  if (execution == QueryExecution::Configured && query_accelerator_ != nullptr) {
    const CorrectionBlend correction = query_accelerator_->blend_at(p, side, query_pricing_tier_);
    if (correction.usable(side)) {
      const double cached =
          american_price_cached(pricing_.S, p.K, p.T, p.sigma, p.rate, p.q_eff, side, correction);
      if (std::isfinite(cached)) {
        telemetry_sample.record_cache_hit(query_pricing_tier_ ==
                                          QueryPricingTier::RepresentativeFast);
        return Ok(cached);
      }
    }
  }
  return american_price(pricing_.S, p.K, p.T, p.sigma, p.rate, p.q_eff, side, pricing_.method,
                        std::optional<AlOpts>{pricing_.al_opts});
}

Result<AmericanGreeks> PricedSurface::greeks_resolved(const ResolvedSurfacePoint &p, Side side,
                                                      bool analytic, QueryExecution execution,
                                                      GreekNeeds needs) const {
  counters::lightweight::QuerySample telemetry_sample{execution == QueryExecution::Configured &&
                                                      query_accelerator_ != nullptr};
  if (execution == QueryExecution::Configured && query_accelerator_ != nullptr) {
    const CorrectionBlend correction = query_accelerator_->blend_at(p, side, query_pricing_tier_);
    if (correction.usable(side)) {
      // L4/K4: the cached-correction route ignores `needs` and returns its full
      // internally-consistent jet — a correctness-preserving superset (a reduced
      // caller reads only the columns it asked for; the extra columns are correct,
      // never stale). This is the rare fast-tier guard corner, not the backtest's
      // cold analytic AL hot path.
      auto cached =
          american_greeks(pricing_.S, p.K, p.T, p.sigma, p.rate, p.q_eff, side, correction);
      if (cached.has_value()) {
        telemetry_sample.record_cache_hit(query_pricing_tier_ ==
                                          QueryPricingTier::RepresentativeFast);
        return cached;
      }
    }
  }
  if (analytic && pricing_.method == AmericanMethod::AndersenLake) {
    // K4 first-order tier (the L4 hot path): map GreekNeeds onto american_greeks_al's
    // need_vega/need_rho/need_charm so a reduced request skips whole boundary solves
    // (full=5, {vega only}=3, {none}=1). The requested columns are BIT-IDENTICAL to the
    // full-bundle values (same base boundary + σ± stencils); unrequested greeks are 0.
    // Default GreekNeeds{} (all true) reproduces the pre-L4 maskless call exactly.
    return american_greeks_al(pricing_.S, p.K, p.T, p.sigma, p.rate, p.q_eff, side,
                              std::optional<AlOpts>{pricing_.al_opts}, needs.vega, needs.rho,
                              needs.charm);
  }
  // FD fallback route: the full oracle (american_greeks_fd is the reference bundle);
  // it ignores `needs` and stays the correctness-preserving superset.
  return american_greeks_fd(pricing_.S, p.K, p.T, p.sigma, p.rate, p.q_eff, side, pricing_.method,
                            std::optional<AlOpts>{pricing_.al_opts});
}

Result<double> PricedSurface::delta_resolved(const ResolvedSurfacePoint &p, Side side,
                                             QueryExecution execution) const {
  counters::lightweight::QuerySample telemetry_sample{execution == QueryExecution::Configured &&
                                                      query_accelerator_ != nullptr};
  if (execution == QueryExecution::Configured && query_accelerator_ != nullptr) {
    const CorrectionBlend correction = query_accelerator_->blend_at(p, side, query_pricing_tier_);
    if (correction.usable(side)) {
      auto cached =
          american_delta(pricing_.S, p.K, p.T, p.sigma, p.rate, p.q_eff, side, correction);
      if (cached.has_value()) {
        telemetry_sample.record_cache_hit(query_pricing_tier_ ==
                                          QueryPricingTier::RepresentativeFast);
        return cached;
      }
    }
  }
  return american_delta(pricing_.S, p.K, p.T, p.sigma, p.rate, p.q_eff, side, pricing_.method,
                        std::optional<AlOpts>{pricing_.al_opts});
}

Result<double> PricedSurface::vega_resolved(const ResolvedSurfacePoint &p, Side side,
                                            QueryExecution execution) const {
  counters::lightweight::QuerySample telemetry_sample{execution == QueryExecution::Configured &&
                                                      query_accelerator_ != nullptr};
  if (execution == QueryExecution::Configured && query_accelerator_ != nullptr) {
    const CorrectionBlend correction = query_accelerator_->blend_at(p, side, query_pricing_tier_);
    if (correction.usable(side)) {
      const double cached =
          american_vega(pricing_.S, p.K, p.T, p.sigma, p.rate, p.q_eff, side, correction);
      if (std::isfinite(cached)) {
        telemetry_sample.record_cache_hit(query_pricing_tier_ ==
                                          QueryPricingTier::RepresentativeFast);
        return Ok(cached);
      }
    }
  }
  if (pricing_.method == AmericanMethod::AndersenLake) {
    return american_vega_al(pricing_.S, p.K, p.T, p.sigma, p.rate, p.q_eff, side,
                            std::optional<AlOpts>{pricing_.al_opts});
  }
  const Result<AmericanGreeks> greeks =
      american_greeks_fd(pricing_.S, p.K, p.T, p.sigma, p.rate, p.q_eff, side, pricing_.method,
                         std::optional<AlOpts>{pricing_.al_opts});
  if (!greeks.has_value()) {
    return Err(greeks.error());
  }
  return Ok(greeks->vega);
}

Result<double> PricedSurface::fair_value(double K, double T, Side side,
                                         QueryExecution execution) const {
  const ResolvedSurfacePoint p = resolve(K, T);
  if (!p.valid) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurface::fair_value: non-finite or non-positive K/T");
  }
  // Cold Andersen-Lake — the override-path re-pricing the session uses. Passing the
  // resolved preset as an engaged optional reproduces the session's own call
  // `american_price(..., in_.deam.al_opts)` exactly (in_ carries the resolved AL
  // opts post-build).
  return price_resolved(p, side, execution);
}

Result<AmericanGreeks> PricedSurface::greeks(double K, double T, Side side,
                                             QueryExecution execution) const {
  const ResolvedSurfacePoint p = resolve(K, T);
  if (!p.valid) {
    return Err(ErrorCode::InvalidArgument, "PricedSurface::greeks: non-finite or non-positive K/T");
  }
  // American Greeks via finite differences on the SAME cold american_price (method
  // + resolved AL preset) fair_value() prices with, so greeks().price == fair_value()
  // bit-identical and the coefficients are American (not the European Black-76 leg).
  // Cold (warm_start=false) keeps greeks bit-reproducible across a surface archive
  // round-trip (the LifecycleIntegration contract); the warm hot path lives in the
  // backtest engine (cross-step reuse), not this bit-stable reprice primitive.
  return greeks_resolved(p, side, false, execution);
}

Result<AmericanGreeks> PricedSurface::greeks_analytic(double K, double T, Side side,
                                                      QueryExecution execution,
                                                      GreekNeeds needs) const {
  const ResolvedSurfacePoint p = resolve(K, T);
  if (!p.valid) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurface::greeks_analytic: non-finite or non-positive K/T");
  }
  // Analytic Andersen-Lake greeks (5 solves; theta/charm via the continuation PDE).
  // Same base boundary as fair_value(), so greeks_analytic().price == fair_value().
  // BAW / degenerate corners fall back to the cold FD path inside american_greeks_al.
  // Fast tiers differentiate their cached surrogate directly, so the analytic
  // flag has no alternate numerical meaning there. Cold serving retains the
  // Andersen-Lake/PDE route. `needs` narrows that cold AL bundle (K4 tier).
  return greeks_resolved(p, side, true, execution, needs);
}

Result<FullGreekSeed> PricedSurface::full_greek_seed(double K, double T, Side side, bool analytic,
                                                     QueryExecution execution) const {
  using EF = EvalField;
  constexpr EF kSeedFields = EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder;
  // WS-P1: produce the seed through the ONE-ELEMENT evaluate_batch rather than the scalar
  // per-contract evaluate(). A seed's entire contract is to reproduce what the batch would
  // have solved, and since P1a the batch dispatches LANED (AVX2) analytic greeks under
  // Auto. The laned kernels are pack-composition invariant — a 1-lane pack returns exactly
  // what a 4-lane pack returns, proved bit-for-bit by
  // PricedSurface.EvaluateBatchLanedGreeksPackCompositionInvariant — so seeding through the
  // same route keeps `seeded == fresh` BIT-identical instead of degrading that contract to
  // a tolerance. Threads the same default ISA (Auto) evaluate_batch itself defaults to;
  // FullGreekSeed carries no ISA, so acceptance is unaffected. On a non-AVX2 host (or a
  // warm/accelerated tier) the gate falls back to the scalar loop and this is byte-for-byte
  // the pre-P1 seed.
  const std::array<double, 1> Ks{K};
  const std::array<double, 1> Ts{T};
  const std::array<Side, 1> sides{side};
  std::array<double, 1> seed_iv{};
  std::array<double, 1> seed_px{};
  std::array<AmericanGreeks, 1> seed_gk{};
  std::array<Status, 1> seed_st{};
  const Status rc =
      evaluate_batch(Ks, Ts, sides, kSeedFields, analytic,
                     EvaluationSoA{seed_iv, seed_px, seed_gk, seed_st, {}, {}},
                     simd::SimdIsa::Auto, execution);
  if (!rc.has_value()) {
    return Err(rc.error());
  }
  if (!seed_st[0].has_value()) {
    return Err(seed_st[0].error());
  }
  return FullGreekSeed{uid(),     K,          T,         side, instance_id_,
                       analytic,  execution,  seed_iv[0], seed_gk[0]};
}

Result<double> PricedSurface::delta(double K, double T, Side side, QueryExecution execution) const {
  const ResolvedSurfacePoint p = resolve(K, T);
  if (!p.valid) {
    return Err(ErrorCode::InvalidArgument, "PricedSurface::delta: non-finite or non-positive K/T");
  }
  // Delta-only fast path — same (S, sigma, r, q_eff, method, al_opts) plumbing as
  // greeks(), so this returns greeks().delta bit-identically at ~1-2 boundary solves
  // instead of seventeen (see american_delta).
  return delta_resolved(p, side, execution);
}

Result<double> PricedSurface::vega(double K, double T, Side side, QueryExecution execution) const {
  const ResolvedSurfacePoint p = resolve(K, T);
  if (!p.valid) {
    return Err(ErrorCode::InvalidArgument, "PricedSurface::vega: non-finite or non-positive K/T");
  }
  return vega_resolved(p, side, execution);
}

PricedSurface::FusedResult PricedSurface::evaluate_resolved(const ResolvedSurfacePoint &p,
                                                            Side side, EvalField fields,
                                                            bool analytic, QueryExecution execution,
                                                            GreekNeeds needs) const {
  FusedResult r;
  if (!p.valid) {
    r.iv = kNaN;
    r.price = kNaN;
    poison(r.greeks);
    r.status =
        Err(ErrorCode::InvalidArgument, "PricedSurface::evaluate: non-finite or non-positive K/T");
    return r;
  }
  // IV is free from the single resolution — always populated when valid.
  r.iv = p.sigma;
  // Poison selective outputs up front so an earlier requested-route failure
  // cannot leave a requested axis looking like a valid zero.
  if (has_field(fields, EvalField::Delta)) {
    r.greeks.delta = kNaN;
  }
  if (has_field(fields, EvalField::Vega)) {
    r.greeks.vega = kNaN;
  }

  // H2 [WS-H STRETCH — DEFERRED]: first-order greek tier end-to-end. Today
  // FirstOrder and SecondOrder COLLAPSE to the same full bundle here (both set
  // want_greeks and call greeks_resolved -> the full american_greeks_al 5-solve /
  // american_greeks_fd 17-solve bundle). A delta-only hedge cadence therefore pays
  // the full second-order bundle. The kernel primitives already exist and are
  // wired-but-unused for this narrowing:
  //   * american_greeks_al(...,need_vega,need_rho,need_charm) skips the sigma+/-
  //     (vega/volga/vanna), r+/- (rho) and wide-speed (charm) boundary solves — a
  //     {delta} bundle is 1 boundary solve vs 5 (see american.hpp; K4 selectors).
  //   * simd::american_put_greeks_batch takes the same need_* selectors (H1 already
  //     forwards all-true; a first-order caller would pass need_*=false).
  // TO FINISH (design): (1) add a first-order request bit to EvalField-consuming
  // callers — either a granular GreekNeeds on PriceOptions or a PriceFieldMask
  // FirstOrder bit (portfolio_pricer.hpp) mapped to EvalField::FirstOrder-only;
  // (2) here, derive need_vega = has(SecondOrder) || <vega requested>, need_rho,
  // need_charm from the requested axes and forward them into greeks_resolved ->
  // american_greeks_al(...,need_*) AND the H1 laned path; leave price+delta+gamma+
  // theta on the base solve. (3) prove a delta-only request drops the sigma/r/speed
  // solves via the BoundarySolves ledger. NOTE: this does NOT move the dispersion
  // benchmark (it prices FullGreeks every step) — it is a general hedge-cadence win.
  // The EvalField::Delta / EvalField::Vega selective primitives + american_delta
  // (1-2 solves) already exist in evaluate_resolved below (honored) but have no
  // portfolio caller — the same wiring exposes them.
  const bool want_greeks =
      has_field(fields, EvalField::FirstOrder) || has_field(fields, EvalField::SecondOrder);
  if (want_greeks) {
    // Route exactly as greeks() / greeks_analytic() do; american_greeks_*().price
    // IS the fair value (bit-identical), so Greeks yield the mark for free.
    ATX_VOL_COUNT(SurfaceFullGreekRoutes);
    // K4 tier: `needs` skips the σ±/r±/charm solves the requested columns don't need
    // (default {} = full 5-solve bundle, bit-identical to pre-L4). `price` (== fair
    // value) still rides the base boundary, so a reduced request keeps `r.price`.
    Result<AmericanGreeks> g = greeks_resolved(p, side, analytic, execution, needs);
    if (!g.has_value()) {
      r.iv = kNaN;
      r.price = kNaN;
      poison(r.greeks);
      r.status = Err(g.error());
      return r;
    }
    // FIX-3/F3-A: stamp with the SAME semantics the laned driver uses (FIX-2/F2-B
    // c601504, itself matching FIX-1's 740b040 / 9c3e1d0). Before this the stamp here
    // was an unconditional default-constructed Ok, so one identical lane came back Ok
    // on ForceScalar / a non-AVX2 host / the FD route and DEMOTED under the laned
    // route — an ISA-dependent status on a live pricing input, the same defect shape
    // rev-ws-g found in the put/call unrequested-Greek asymmetry.
    //
    // Reachability, which is why BOTH halves are needed here: the FD route
    // (american_greeks_fd takes no mask) and the cached-correction route both IGNORE
    // `needs` and return the full oracle bundle, and the cold AL route leaves an
    // unrequested slot NON-finite rather than 0. So an unrequested column really can
    // arrive here non-finite — guarding the full bundle would veto a lane on a column
    // the caller never asked for (FIX-1/F3's over-guard defect), and merely widening
    // the mask without normalizing would hand a NaN to a consumer whose `g.rho * dr`
    // is NaN even at dr == 0.0. Normalization is restricted to the non-finite case, so
    // every lane admitted today is bit-for-bit unchanged.
    AmericanGreeks gg = *g;
    detail::normalize_unrequested_greeks(gg, needs);
    r.greeks = gg;
    r.price = gg.price;
    if (!detail::requested_greeks_finite(gg, needs)) {
      // NaN-isolated by STATUS, exactly as the laned stamp and FIX-1's portfolio
      // stamps do; the computed columns are left in place for diagnosis because every
      // consumer gates on status.
      r.status = Err(ErrorCode::Internal,
                     "PricedSurface::evaluate: non-finite price or requested Greek");
    }
    return r;
  }
  if (has_field(fields, EvalField::Price)) {
    ATX_VOL_COUNT(SurfaceScalarPriceRoutes);
    Result<double> fv = price_resolved(p, side, execution);
    if (!fv.has_value()) {
      r.iv = kNaN;
      r.price = kNaN;
      poison(r.greeks);
      r.status = Err(fv.error());
      return r;
    }
    r.price = *fv;
  }
  if (has_field(fields, EvalField::Delta)) {
    ATX_VOL_COUNT(SurfaceDeltaRoutes);
    const Result<double> delta = delta_resolved(p, side, execution);
    if (!delta.has_value()) {
      r.iv = kNaN;
      r.price = kNaN;
      poison(r.greeks);
      r.status = Err(delta.error());
      return r;
    }
    r.greeks.delta = *delta;
  }
  if (has_field(fields, EvalField::Vega)) {
    ATX_VOL_COUNT(SurfaceVegaRoutes);
    Result<double> vega_result = vega_resolved(p, side, execution);
    if (!vega_result.has_value()) {
      r.iv = kNaN;
      r.price = kNaN;
      poison(r.greeks);
      r.status = Err(vega_result.error());
      return r;
    }
    r.greeks.vega = *vega_result;
  }
  // Iv-only (or None): one resolution, no pricer solve.
  return r;
}

PricedSurface::FusedResult PricedSurface::evaluate(double K, double T, Side side, EvalField fields,
                                                   bool analytic, QueryExecution execution,
                                                   GreekNeeds needs) const {
  return evaluate_resolved(resolve(K, T), side, fields, analytic, execution, needs);
}

Status PricedSurface::evaluate_batch(std::span<const double> K, std::span<const double> T,
                                     std::span<const Side> side, EvalField fields, bool analytic,
                                     EvaluationSoA out, simd::SimdIsa resolved_price_isa,
                                     QueryExecution execution, GreekNeeds needs) const {
  const std::size_t n = K.size();
  if (T.size() != n || side.size() != n) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurface::evaluate_batch: K/T/side length mismatch");
  }
  const bool want_greeks =
      has_field(fields, EvalField::FirstOrder) || has_field(fields, EvalField::SecondOrder);
  const bool want_price = has_field(fields, EvalField::Price);
  const bool want_iv = has_field(fields, EvalField::Iv);
  const bool want_delta = has_field(fields, EvalField::Delta);
  const bool want_vega = has_field(fields, EvalField::Vega);
  const bool selective_only = !want_greeks && (want_delta || want_vega);
  const auto optional_size_ok = [n](const auto output) noexcept {
    return output.empty() || output.size() == n;
  };
  if (!selective_only && (out.iv.size() != n || out.price.size() != n || out.status.size() != n)) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurface::evaluate_batch: iv/price/status out-span size != query count");
  }
  if (!optional_size_ok(out.delta) || !optional_size_ok(out.vega) ||
      (want_delta && out.delta.size() != n) || (want_vega && out.vega.size() != n)) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurface::evaluate_batch: dedicated axis out-span size invalid");
  }
  if (selective_only &&
      (out.status.size() != n || !optional_size_ok(out.iv) || !optional_size_ok(out.price) ||
       !optional_size_ok(out.greeks) || (want_iv && out.iv.size() != n) ||
       (want_price && out.price.size() != n))) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurface::evaluate_batch: selective out-span size invalid");
  }
  if (want_greeks && out.greeks.size() != n) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurface::evaluate_batch: greeks out-span size != query count");
  }
  const auto overlaps_input = [&](const auto output) noexcept {
    return spans_overlap(K, output) || spans_overlap(T, output) || spans_overlap(side, output);
  };
  const bool outputs_overlap =
      spans_overlap(out.iv, out.price) || spans_overlap(out.iv, out.greeks) ||
      spans_overlap(out.iv, out.delta) || spans_overlap(out.iv, out.vega) ||
      spans_overlap(out.iv, out.status) || spans_overlap(out.price, out.greeks) ||
      spans_overlap(out.price, out.delta) || spans_overlap(out.price, out.vega) ||
      spans_overlap(out.price, out.status) || spans_overlap(out.greeks, out.delta) ||
      spans_overlap(out.greeks, out.vega) || spans_overlap(out.greeks, out.status) ||
      spans_overlap(out.delta, out.vega) || spans_overlap(out.delta, out.status) ||
      spans_overlap(out.vega, out.status);
  if (overlaps_input(out.iv) || overlaps_input(out.price) || overlaps_input(out.greeks) ||
      overlaps_input(out.status) || overlaps_input(out.delta) || overlaps_input(out.vega) ||
      outputs_overlap) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurface::evaluate_batch: query/output spans overlap");
  }

  std::size_t i = 0;
  while (i < n) {
    const double t = T[i];
    // A run [i, j) of BIT-identical T (raw double compare, no tolerance) shares
    // one T-bracket + carry — the ladder-reuse win.
    std::size_t j = i + 1;
    while (j < n && std::bit_cast<std::uint64_t>(T[j]) == std::bit_cast<std::uint64_t>(t)) {
      ++j;
    }
    const bool t_valid = std::isfinite(t) && (t > 0.0);
    const ForwardCarry fc = t_valid ? interp_forward(t) : ForwardCarry{};
    const CurveSurface::Bracket surface_bracket =
        t_valid ? surface_.bracket(t) : CurveSurface::Bracket{};
    // The resolved batch dispatcher is a cold-American kernel and accepts no
    // correction state. Fast tiers stay on evaluate_resolved so a price-only
    // ladder cannot silently bypass its configured surrogate; the call-local
    // ISA selector is therefore meaningful only on the cold path.
    if (want_price && !want_greeks && !selective_only &&
        (execution == QueryExecution::ColdReference || query_accelerator_ == nullptr)) {
      if (!t_valid) {
        for (std::size_t e = i; e < j; ++e) {
          ResolvedSurfacePoint p;
          p.K = K[e];
          p.T = t;
          const FusedResult fr = evaluate_resolved(p, side[e], fields, analytic, execution, needs);
          out.iv[e] = fr.iv;
          out.price[e] = fr.price;
          out.status[e] = fr.status;
        }
        i = j;
        continue;
      }

      const auto dispatch_valid = [&](std::size_t begin, std::size_t end) -> Status {
        if (begin == end) {
          return Ok();
        }
        const std::size_t run_size = end - begin;
        const ResolvedAmericanPriceBatchRequest request{
            .S = pricing_.S,
            .T = t,
            .r = fc.rate,
            .q = fc.q_eff,
            .K = K.subspan(begin, run_size),
            .sigma = out.iv.subspan(begin, run_size),
            .side = side.subspan(begin, run_size),
            .method = pricing_.method,
            .al_opts = std::optional<AlOpts>{pricing_.al_opts},
            .isa = resolved_price_isa,
            .price = out.price.subspan(begin, run_size),
            .status = out.status.subspan(begin, run_size),
            .pack_dispatch = {},
        };
        const Status batch = american_price_batch_resolved(request);
        if (!batch.has_value()) {
          return batch;
        }
        // Mirror the scalar reference path (evaluate_resolved): an american_price
        // failure poisons iv = NaN. The batch pre-filled out.iv[e] = p.sigma for
        // every valid resolution; on lanes the pricer rejected (status Err, price
        // NaN) the scalar route overwrites iv with NaN, so poison here to keep the
        // iv column bit-identical to the per-contract evaluate.
        for (std::size_t e = begin; e < end; ++e) {
          if (!out.status[e].has_value()) {
            out.iv[e] = kNaN;
          }
        }
        return Ok();
      };

      std::size_t valid_begin = i;
      for (std::size_t e = i; e < j; ++e) {
        const ResolvedSurfacePoint p = resolve_with_carry_and_bracket(K[e], t, fc, surface_bracket);
        if (p.valid) {
          out.iv[e] = p.sigma;
          continue;
        }
        const Status batch_status = dispatch_valid(valid_begin, e);
        if (!batch_status.has_value()) {
          return batch_status;
        }
        const FusedResult fr = evaluate_resolved(p, side[e], fields, analytic, execution, needs);
        out.iv[e] = fr.iv;
        out.price[e] = fr.price;
        out.status[e] = fr.status;
        valid_begin = e + 1;
      }
      const Status batch_status = dispatch_valid(valid_begin, j);
      if (!batch_status.has_value()) {
        return batch_status;
      }
      i = j;
      continue;
    }
    // WS-P1 (P1a+P1b): laned ANALYTIC American Greeks under production Auto ISA, BOTH
    // sides. Dispatch this run's valid PUT lanes through simd::american_put_greeks_batch
    // and valid CALL lanes through simd::american_call_greeks_batch (the P1b call-native
    // mirror), instead of the per-contract scalar american_greeks_al fan. THIS surface's
    // al_opts is threaded through — the higher-level american_greeks_batch forces
    // std::nullopt (a DIFFERENT, more expensive AlScheme than the reloaded surfaces'
    // al_fast_opts), so it is NOT a byte-identical drop-in. Each kernel patches every
    // non-early-exercise / non-finite lane through the exact scalar american_greeks_al
    // (...,al_opts) — byte-identical to greeks_resolved's cold branch; genuine early-
    // exercise lanes ride the 4-wide vector bundle.
    //
    // P1a GATE FLIP (PM-locked, economic-parity decision): the dispatch predicate is now
    // simd::avx2_greeks_selected(isa), NOT the old resolved_price_isa == ForceAvx2. Under
    // production Auto on an AVX2 host kShipAvx2Greeks is true, so this run's base-greek
    // bundles (≈83% of dispersion solve volume — 1 straddle/name) now RIDE the laned
    // kernel instead of silently falling to the scalar per-contract loop. The cost: the
    // laned greeks differ from scalar by ~1e-13/greek (the AVX2 transcendentals in the
    // stencil prices amplified by the FD denominators), so the historical evaluate_batch
    // == per-entry-evaluate BIT-identity is RELAXED to a documented numeric tolerance
    // (greeks are consumed at ~1e-6 economic precision; the shift is 10+ orders below a
    // tick — sub-economic). The batch==scalar parity tests were re-gated to that
    // tolerance. ForceScalar (and non-AVX2 hosts) keep the exact scalar loop; a single-
    // contract evaluate() still uses scalar american_greeks_al, so per-entry vs batch now
    // agree only within the gate, by design. Determinism: pack membership is fixed by the
    // strike order within a single-thread [i,j) run, independent of any pricing-executor
    // thread partition. Cold path only (mirror the marks arm's accelerator guard).
    //
    // WS-P1v: the pack/flush/scatter body now lives in ONE place —
    // detail::laned_greek_run (src/laned_greek_run.hpp) — shared verbatim with
    // PricedSurfaceView::evaluate_batch, which was a pure scalar per-entry Greek loop
    // until this change. Only the resolution and the scalar-fallback routing differ
    // between the two types; both are passed in as lambdas below.
    if (detail::laned_greek_route_selected(want_greeks, selective_only, want_delta, want_vega,
                                           analytic, t_valid, pricing_.method,
                                           resolved_price_isa) &&
        (execution == QueryExecution::ColdReference || query_accelerator_ == nullptr)) {
      detail::laned_greek_run(
          pricing_.S, i, j, side, std::optional<AlOpts>{pricing_.al_opts}, resolved_price_isa,
          needs, out,
          [&](std::size_t e) {
            return resolve_with_carry_and_bracket(K[e], t, fc, surface_bracket);
          },
          [&](std::size_t e, Side sd) {
            // FIX-3/F3-A: `needs` MUST be threaded. Without it the fallback recomputed
            // the full default bundle and was then judged under full-bundle finiteness,
            // i.e. a narrowed caller's lane could be vetoed on a column it never
            // requested — FIX-1/F3's over-guard defect, reintroduced on the fallback
            // path. It also restores surface/view parity: the view's identical lambda
            // (priced_surface_view.cpp) has always passed `needs`.
            return evaluate_resolved(resolve_with_carry_and_bracket(K[e], t, fc, surface_bracket),
                                     sd, fields, analytic, execution, needs);
          });
      i = j;
      continue;
    }
    for (std::size_t e = i; e < j; ++e) {
      // Bit-identical to evaluate(K[e], t, ...): resolve_with_carry(K,t,interp_forward(t))
      // == resolve(K,t), and evaluate_resolved is the shared routing.
      ResolvedSurfacePoint p;
      if (t_valid) {
        p = resolve_with_carry_and_bracket(K[e], t, fc, surface_bracket);
      } else {
        p.K = K[e];
        p.T = t; // valid == false
      }
      const FusedResult fr = evaluate_resolved(p, side[e], fields, analytic, execution, needs);
      if (!selective_only || want_iv) {
        out.iv[e] = fr.iv;
      }
      if (!selective_only || want_price) {
        out.price[e] = fr.price;
      }
      if (want_greeks) {
        out.greeks[e] = fr.greeks;
      }
      if (want_delta) {
        out.delta[e] = fr.greeks.delta;
      }
      if (want_vega) {
        out.vega[e] = fr.greeks.vega;
      }
      out.status[e] = fr.status;
    }
    i = j;
  }
  return Ok();
}

VolCurveKind PricedSurface::kind_at(std::size_t i) const noexcept {
  return surface_.slices()[i]->kind();
}

} // namespace atx::vol
