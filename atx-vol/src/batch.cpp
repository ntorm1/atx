// Public, validated span adapters over the runtime-dispatched AVX2 kernels.
// The vector routes use the existing scalar patch-through for degenerate,
// deep-wing, and ill-conditioned lanes. Scalar fallback remains authoritative
// on hosts without AVX2+FMA and for the from-lnFK kernel, which has no vector
// implementation. Outputs are required to be disjoint from every input because
// the vector kernels load and store in four-lane blocks.

#include "atx/vol/batch.hpp"

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <limits>

#include "atx/vol/simd/cpu.hpp"

#include "simd/black76_batch_avx2.hpp"
#include "simd/essvi_batch_avx2.hpp"
#include "simd/greeks_batch_avx2.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

namespace {

// True iff every span length in `sizes` is equal (an empty list is vacuously
// equal). Used to enforce the "all per-lane spans share one length" contract
// at each boundary. Pure, no allocation.
[[nodiscard]] bool sizes_match(std::initializer_list<std::size_t> sizes) noexcept {
  if (sizes.size() == 0) {
    return true;
  }
  const std::size_t n = *sizes.begin();
  for (const std::size_t s : sizes) {
    if (s != n) {
      return false;
    }
  }
  return true;
}

// std::less<void*> supplies the strict total order that built-in relational
// pointer operators do not provide for unrelated allocations.
template <typename L, typename R>
[[nodiscard]] bool spans_overlap(std::span<L> lhs, std::span<R> rhs) noexcept {
  if (lhs.empty() || rhs.empty()) {
    return false;
  }
  const auto less = std::less<const void *>{};
  const void *const lhs_begin = static_cast<const void *>(lhs.data());
  const void *const lhs_end = static_cast<const void *>(lhs.data() + lhs.size());
  const void *const rhs_begin = static_cast<const void *>(rhs.data());
  const void *const rhs_end = static_cast<const void *>(rhs.data() + rhs.size());
  return less(lhs_begin, rhs_end) && less(rhs_begin, lhs_end);
}

template <typename Output, typename... Inputs>
[[nodiscard]] bool overlaps_any(std::span<Output> output, std::span<Inputs>... inputs) noexcept {
  return (spans_overlap(output, inputs) || ...);
}

constexpr std::size_t kAvx2LaneWidth = 4;
// The eSSVI backbone is much cheaper per lane than the pricing kernels. Its
// measured 2.8x win is on large grids; stay scalar for tiny spans until their
// setup crossover is independently benchmarked.
constexpr std::size_t kEssviAvx2MinBatch = 16;

} // namespace

Status black76_price_batch(std::span<const double> F, std::span<const double> K,
                           std::span<const double> T, std::span<const double> sigma,
                           std::span<const double> df, std::span<const Side> side,
                           std::span<double> price_out) {
  if (!sizes_match(
          {F.size(), K.size(), T.size(), sigma.size(), df.size(), side.size(), price_out.size()})) {
    return Err(ErrorCode::InvalidArgument, "black76_price_batch: span length mismatch");
  }
  if (overlaps_any(price_out, F, K, T, sigma, df, side)) {
    return Err(ErrorCode::InvalidArgument, "black76_price_batch: output aliases input");
  }
  const std::size_t n = F.size();
  if (n >= kAvx2LaneWidth && simd::have_avx2()) {
    simd::detail::black76_price_batch_avx2(F.data(), K.data(), T.data(), sigma.data(), df.data(),
                                           side.data(), price_out.data(), n);
    return Ok();
  }
  for (std::size_t i = 0; i < n; ++i) {
    price_out[i] = black76_price(F[i], K[i], T[i], sigma[i], df[i], side[i]);
  }
  return Ok();
}

Status black76_price_from_lnfk_batch(std::span<const double> F, std::span<const double> K, double T,
                                     double sqrt_t, std::span<const double> sigma, double df,
                                     std::span<const double> ln_fk, std::span<const Side> side,
                                     std::span<double> price_out) {
  if (!sizes_match(
          {F.size(), K.size(), sigma.size(), ln_fk.size(), side.size(), price_out.size()})) {
    return Err(ErrorCode::InvalidArgument, "black76_price_from_lnfk_batch: span length mismatch");
  }
  if (overlaps_any(price_out, F, K, sigma, ln_fk, side)) {
    return Err(ErrorCode::InvalidArgument, "black76_price_from_lnfk_batch: output aliases input");
  }
  const std::size_t n = F.size();
  for (std::size_t i = 0; i < n; ++i) {
    price_out[i] = black76_price_from_lnfk(F[i], K[i], T, sigma[i], df, ln_fk[i], sqrt_t, side[i]);
  }
  return Ok();
}

Status black76_value_and_vega_batch(std::span<const double> F, std::span<const double> K, double T,
                                    std::span<const double> sigma, std::span<const double> df,
                                    std::span<const Side> side, std::span<double> value_out,
                                    std::span<double> vega_out, double sqrt_t_in) {
  if (!sizes_match({F.size(), K.size(), sigma.size(), df.size(), side.size(), value_out.size(),
                    vega_out.size()})) {
    return Err(ErrorCode::InvalidArgument, "black76_value_and_vega_batch: span length mismatch");
  }
  if (overlaps_any(value_out, F, K, sigma, df, side, vega_out) ||
      overlaps_any(vega_out, F, K, sigma, df, side)) {
    return Err(ErrorCode::InvalidArgument,
               "black76_value_and_vega_batch: outputs alias input or each "
               "other");
  }
  const std::size_t n = F.size();
  // A caller-supplied zero sqrt(T) intentionally produces the scalar kernel's
  // degenerate arithmetic. Keep that unusual sentinel off the vector route.
  if (n >= kAvx2LaneWidth && sqrt_t_in != 0.0 && simd::have_avx2()) {
    simd::detail::black76_value_vega_shared_t_batch_avx2(F.data(), K.data(), T, sqrt_t_in,
                                                         sigma.data(), df.data(), side.data(),
                                                         value_out.data(), vega_out.data(), n);
    return Ok();
  }
  for (std::size_t i = 0; i < n; ++i) {
    const Black76ValueVega vv =
        black76_value_and_vega(F[i], K[i], T, sigma[i], df[i], side[i], sqrt_t_in);
    value_out[i] = vv.price;
    vega_out[i] = vv.vega;
  }
  return Ok();
}

Status implied_vol_batch(std::span<const double> price, std::span<const double> F,
                         std::span<const double> K, std::span<const double> T,
                         std::span<const double> df, std::span<const Side> side,
                         std::span<double> iv_out, std::span<Status> status_out) {
  if (!sizes_match({price.size(), F.size(), K.size(), T.size(), df.size(), side.size(),
                    iv_out.size(), status_out.size()})) {
    return Err(ErrorCode::InvalidArgument, "implied_vol_batch: span length mismatch");
  }
  if (overlaps_any(iv_out, price, F, K, T, df, side, status_out) ||
      overlaps_any(status_out, price, F, K, T, df, side)) {
    return Err(ErrorCode::InvalidArgument, "implied_vol_batch: outputs alias input or each other");
  }
  const std::size_t n = price.size();
  for (std::size_t i = 0; i < n; ++i) {
    Result<double> iv = implied_vol(price[i], F[i], K[i], T[i], df[i], side[i]);
    if (iv) {
      iv_out[i] = *iv;
      status_out[i] = Ok();
    } else {
      // Match the C batch's NaN-on-fail value slot + parallel status.
      iv_out[i] = std::numeric_limits<double>::quiet_NaN();
      status_out[i] = Err(iv.error());
    }
  }
  return Ok();
}

Status black76_greeks_batch(std::span<const double> F, std::span<const double> K,
                            std::span<const double> T, std::span<const double> sigma,
                            std::span<const double> r, std::span<const double> df,
                            std::span<const Side> side, std::span<Greeks> greeks_out,
                            std::span<double> price_out) {
  if (!sizes_match({F.size(), K.size(), T.size(), sigma.size(), r.size(), df.size(), side.size(),
                    greeks_out.size()})) {
    return Err(ErrorCode::InvalidArgument, "black76_greeks_batch: span length mismatch");
  }
  const bool want_price = !price_out.empty();
  if (want_price && price_out.size() != F.size()) {
    return Err(ErrorCode::InvalidArgument, "black76_greeks_batch: price_out length mismatch");
  }
  if (overlaps_any(greeks_out, F, K, T, sigma, r, df, side, price_out) ||
      overlaps_any(price_out, F, K, T, sigma, r, df, side)) {
    return Err(ErrorCode::InvalidArgument,
               "black76_greeks_batch: outputs alias input or each other");
  }
  const std::size_t n = F.size();
  if (n >= kAvx2LaneWidth && simd::have_avx2()) {
    simd::detail::black76_greeks_batch_avx2(F.data(), K.data(), T.data(), sigma.data(), r.data(),
                                            df.data(), side.data(), greeks_out.data(),
                                            want_price ? price_out.data() : nullptr, n);
    return Ok();
  }
  for (std::size_t i = 0; i < n; ++i) {
    const Black76Greeks g = black76_greeks(F[i], K[i], T[i], sigma[i], r[i], df[i], side[i]);
    greeks_out[i] = g.greeks;
    if (want_price) {
      price_out[i] = g.price;
    }
  }
  return Ok();
}

Status essvi_w_batch(const EssviSlice &slice, std::span<const double> k_log,
                     std::span<double> w_out) {
  if (k_log.size() != w_out.size()) {
    return Err(ErrorCode::InvalidArgument, "essvi_w_batch: span length mismatch");
  }
  if (spans_overlap(k_log, w_out)) {
    return Err(ErrorCode::InvalidArgument, "essvi_w_batch: output aliases input");
  }
  const std::size_t n = k_log.size();
  if (n >= kEssviAvx2MinBatch && simd::have_avx2()) {
    EssviParams params{};
    params.theta = slice.theta;
    params.phi = slice.phi;
    params.rho = slice.rho;
    params.rho_R = slice.rho;
    params.T = slice.T;
    simd::detail::essvi_backbone_w_batch_avx2(params, k_log.data(), w_out.data(), n);
    return Ok();
  }
  for (std::size_t i = 0; i < n; ++i) {
    w_out[i] = essvi_w(slice, k_log[i]);
  }
  return Ok();
}

} // namespace atx::vol
