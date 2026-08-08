#include "atx/vol/theo.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"       // american_price, AlOpts, AmericanMethod
#include "atx/vol/priced_surface.hpp" // PricedSurface

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
} // namespace

TheoEngine::TheoEngine(std::vector<std::unique_ptr<ITheoOverlay>> ovs, const TheoConfig &c)
    : overlays_(std::move(ovs)), cfg_(c) {}

Result<TheoEngine> TheoEngine::create(std::vector<std::unique_ptr<ITheoOverlay>> overlays,
                                      const TheoConfig &cfg) {
  for (const std::unique_ptr<ITheoOverlay> &overlay : overlays) {
    if (overlay == nullptr) {
      return Err(ErrorCode::InvalidArgument, "TheoEngine::create: null overlay");
    }
  }
  if (!(cfg.max_abs_dvol > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "TheoEngine::create: max_abs_dvol must be > 0");
  }
  if (!(cfg.band_floor_vol >= 0.0)) {
    return Err(ErrorCode::InvalidArgument, "TheoEngine::create: band_floor_vol must be >= 0");
  }
  return Ok(TheoEngine(std::move(overlays), cfg));
}

Result<TheoValue> TheoEngine::value(const TheoContext &ctx, const TheoQuery &q) const {
  const std::array<TheoQuery, 1> qs{q};
  std::array<TheoValue, 1> out{};
  const Status st = value_into(ctx, qs, out);
  if (!st.has_value()) {
    return Err(st.error());
  }
  return Ok(out[0]);
}

Status TheoEngine::value_into(const TheoContext &ctx, std::span<const TheoQuery> qs,
                              std::span<TheoValue> out) const {
  if (out.size() != qs.size()) {
    return Err(ErrorCode::InvalidArgument, "TheoEngine::value_into: out span size != query count");
  }
  if (ctx.surface == nullptr) {
    return Err(ErrorCode::InvalidArgument, "TheoEngine::value_into: ctx.surface is null");
  }
  const PricedSurface &surface = *ctx.surface;

  std::size_t begin = 0;
  while (begin < qs.size()) {
    const std::size_t n = std::min(kTheoMaxBatch, qs.size() - begin);
    const std::span<const TheoQuery> chunk_q = qs.subspan(begin, n);
    const std::span<TheoValue> chunk_out = out.subspan(begin, n);

    // Baseline: one surface read per query. theo_vol seeds at market_vol so a
    // chunk with no engaged overlays is bit-for-bit the served mark already
    // (the identity contract) -- nothing below mutates it further.
    for (std::size_t i = 0; i < n; ++i) {
      const TheoQuery &q = chunk_q[i];
      const Result<double> market_price = surface.fair_value(q.strike, q.tenor_years, q.side);
      if (!market_price.has_value()) {
        return Err(market_price.error());
      }
      TheoValue &v = chunk_out[i];
      v = TheoValue{};
      v.market_vol = surface.iv(q.strike, q.tenor_years);
      v.market_price = *market_price;
      v.theo_vol = v.market_vol;
    }

    // Fixed-capacity, no-allocation overlay scratch -- reused across every
    // overlay in this chunk (and across chunks).
    std::array<OverlayAdjust, kTheoMaxBatch> scratch{};
    const std::span<OverlayAdjust> scratch_span{scratch.data(), n};

    for (const std::unique_ptr<ITheoOverlay> &overlay : overlays_) {
      for (std::size_t i = 0; i < n; ++i) {
        scratch_span[i] = OverlayAdjust{};
      }
      const Status st = overlay->adjust(ctx, chunk_q, scratch_span);
      if (!st.has_value()) {
        return Err(st.error());
      }
      for (std::size_t i = 0; i < n; ++i) {
        const double raw_dvol = scratch_span[i].dvol;
        const double clamped_dvol = std::clamp(raw_dvol, -cfg_.max_abs_dvol, cfg_.max_abs_dvol);
        if (clamped_dvol != raw_dvol) {
          chunk_out[i].flags |= static_cast<std::uint32_t>(TheoFlagBits::OverlayClamped);
        }
        chunk_out[i].theo_vol += clamped_dvol;
        const double band = scratch_span[i].band;
        chunk_out[i].band_vol += band * band; // running sum-of-squares; finalized below
      }
    }

    // Finalize: edge_vol, band_vol (sum-of-squares -> floored sqrt), and
    // theo_price (identity reuse, skipped reprice, or a fresh American solve).
    for (std::size_t i = 0; i < n; ++i) {
      TheoValue &v = chunk_out[i];
      v.edge_vol = v.market_vol - v.theo_vol;
      v.band_vol = std::max(cfg_.band_floor_vol, std::sqrt(v.band_vol));

      if (v.theo_vol == v.market_vol) {
        // Zero net overlay adjustment: reuse the already-computed market
        // price bit-for-bit rather than a second, independently-rounded
        // American solve at the "same" vol -- the identity contract.
        v.theo_price = v.market_price;
        continue;
      }
      if (!cfg_.price_theo) {
        v.theo_price = kNaN; // vol-space-only sheet: no reprice at a shifted vol
        continue;
      }
      const TheoQuery &q = chunk_q[i];
      const Result<double> theo_price = american_price(
          surface.pricing().S, q.strike, q.tenor_years, v.theo_vol, surface.rate_at(q.tenor_years),
          surface.q_eff_at(q.tenor_years), q.side, surface.pricing().method,
          std::optional<AlOpts>{surface.pricing().al_opts});
      if (!theo_price.has_value()) {
        return Err(theo_price.error());
      }
      v.theo_price = *theo_price;
    }

    begin += n;
  }
  return Ok();
}

} // namespace atx::vol
