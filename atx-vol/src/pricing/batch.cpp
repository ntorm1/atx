// Public, validated span adapters over the runtime-dispatched AVX2 kernels.
// The vector routes use the existing scalar patch-through for degenerate,
// deep-wing, and ill-conditioned lanes. Scalar fallback remains authoritative
// on hosts without AVX2+FMA and for the from-lnFK kernel, which has no vector
// implementation.
//
// R-23 (identity aliasing permitted; Sprint I). Aliasing policy at this public
// boundary:
//   • output == input, EXACT identity (same first byte AND same one-past-end
//     byte): PERMITTED. Every kernel here processes strictly forward, loading a
//     4-lane block (or a scalar element) fully before its same-index store, so
//     an output that exactly coincides with an input reads each value before it
//     is overwritten — the in-place result is bit-identical to the disjoint one.
//     The pre-W1 scalar batch supported this; W1 over-rejected it.
//   • output ↔ input, STAGGERED / partial overlap: REJECTED. A shifted output
//     would read an already-overwritten input element; the forward-block
//     invariant does not cover it.
//   • output ↔ output overlap (any, including exact): REJECTED. Two result
//     streams writing the same storage is always a caller bug.
// Class: pure-refactor — no computed value changes; only the accepted-input set
// widens back to the pre-W1 contract. The batch_test.cpp cases assert both the
// permitted in-place path and the still-rejected staggered/output-output paths.

#include "atx/vol/api/pricing/batch.hpp"

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <limits>

#include "atx/vol/api/simd/cpu.hpp"

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

// True iff `lhs` and `rhs` denote the EXACT same byte range: identical first
// byte AND identical one-past-the-end byte. This is the only aliasing the batch
// kernels support in place — a full-span exact overlap of an output onto an
// input. (Two spans of different element types compare equal here only if their
// byte extents coincide exactly, which the length-matched public APIs never
// produce for distinct buffers.)
template <typename L, typename R>
[[nodiscard]] bool spans_exact_same_range(std::span<L> lhs, std::span<R> rhs) noexcept {
  const void *const lhs_begin = static_cast<const void *>(lhs.data());
  const void *const lhs_end = static_cast<const void *>(lhs.data() + lhs.size());
  const void *const rhs_begin = static_cast<const void *>(rhs.data());
  const void *const rhs_end = static_cast<const void *>(rhs.data() + rhs.size());
  return lhs_begin == rhs_begin && lhs_end == rhs_end;
}

// An output↔input aliasing CONFLICT: the two spans overlap but are NOT the exact
// same byte range. Exact in==out identity is permitted (safe in-place, see the
// file header); any staggered/partial overlap is a conflict and is rejected.
template <typename Output, typename Input>
[[nodiscard]] bool input_alias_conflict(std::span<Output> output,
                                        std::span<Input> input) noexcept {
  return spans_overlap(output, input) && !spans_exact_same_range(output, input);
}

template <typename Output, typename... Inputs>
[[nodiscard]] bool any_input_alias_conflict(std::span<Output> output,
                                            std::span<Inputs>... inputs) noexcept {
  return (input_alias_conflict(output, inputs) || ...);
}

constexpr std::size_t kAvx2LaneWidth = 4;
// The eSSVI backbone is much cheaper per lane than the pricing kernels. Its
// measured 2.8x win is on large grids; stay scalar for tiny spans until their
// setup crossover is independently benchmarked.
constexpr std::size_t kEssviAvx2MinBatch = 16;

} // namespace

Result<std::size_t> black76_price_batch(std::span<const double> F, std::span<const double> K,
                                        std::span<const double> T, std::span<const double> sigma,
                                        std::span<const double> df, std::span<const Side> side,
                                        std::span<double> price_out) {
  if (!sizes_match(
          {F.size(), K.size(), T.size(), sigma.size(), df.size(), side.size(), price_out.size()})) {
    return Err(ErrorCode::InvalidArgument, "black76_price_batch: span length mismatch");
  }
  if (any_input_alias_conflict(price_out, F, K, T, sigma, df, side)) {
    return Err(ErrorCode::InvalidArgument,
               "black76_price_batch: output overlaps an input (only exact in-place aliasing is "
               "permitted)");
  }
  const std::size_t n = F.size();
  if (n >= kAvx2LaneWidth && simd::use_avx2()) {
    simd::detail::black76_price_batch_avx2(F.data(), K.data(), T.data(), sigma.data(), df.data(),
                                           side.data(), price_out.data(), n);
    return Ok(n);
  }
  for (std::size_t i = 0; i < n; ++i) {
    price_out[i] = black76_price(F[i], K[i], T[i], sigma[i], df[i], side[i]);
  }
  return Ok(n);
}

Result<std::size_t> black76_price_from_lnfk_batch(std::span<const double> F,
                                                  std::span<const double> K, double T,
                                                  double sqrt_t, std::span<const double> sigma,
                                                  double df, std::span<const double> ln_fk,
                                                  std::span<const Side> side,
                                                  std::span<double> price_out) {
  if (!sizes_match(
          {F.size(), K.size(), sigma.size(), ln_fk.size(), side.size(), price_out.size()})) {
    return Err(ErrorCode::InvalidArgument, "black76_price_from_lnfk_batch: span length mismatch");
  }
  if (any_input_alias_conflict(price_out, F, K, sigma, ln_fk, side)) {
    return Err(ErrorCode::InvalidArgument,
               "black76_price_from_lnfk_batch: output overlaps an input (only exact in-place "
               "aliasing is permitted)");
  }
  const std::size_t n = F.size();
  for (std::size_t i = 0; i < n; ++i) {
    price_out[i] = black76_price_from_lnfk(F[i], K[i], T, sigma[i], df, ln_fk[i], sqrt_t, side[i]);
  }
  return Ok(n);
}

Result<std::size_t> black76_value_and_vega_batch(std::span<const double> F,
                                                 std::span<const double> K, double T,
                                                 std::span<const double> sigma,
                                                 std::span<const double> df,
                                                 std::span<const Side> side,
                                                 std::span<double> value_out,
                                                 std::span<double> vega_out, double sqrt_t_in) {
  if (!sizes_match({F.size(), K.size(), sigma.size(), df.size(), side.size(), value_out.size(),
                    vega_out.size()})) {
    return Err(ErrorCode::InvalidArgument, "black76_value_and_vega_batch: span length mismatch");
  }
  if (any_input_alias_conflict(value_out, F, K, sigma, df, side) ||
      any_input_alias_conflict(vega_out, F, K, sigma, df, side) ||
      spans_overlap(value_out, vega_out)) {
    return Err(ErrorCode::InvalidArgument,
               "black76_value_and_vega_batch: outputs overlap each other or partially overlap an "
               "input (only exact in-place aliasing is permitted)");
  }
  const std::size_t n = F.size();
  // A caller-supplied zero sqrt(T) intentionally produces the scalar kernel's
  // degenerate arithmetic. Keep that unusual sentinel off the vector route.
  if (n >= kAvx2LaneWidth && sqrt_t_in != 0.0 && simd::use_avx2()) {
    simd::detail::black76_value_vega_shared_t_batch_avx2(F.data(), K.data(), T, sqrt_t_in,
                                                         sigma.data(), df.data(), side.data(),
                                                         value_out.data(), vega_out.data(), n);
    return Ok(n);
  }
  for (std::size_t i = 0; i < n; ++i) {
    const Black76ValueVega vv =
        black76_value_and_vega(F[i], K[i], T, sigma[i], df[i], side[i], sqrt_t_in);
    value_out[i] = vv.price;
    vega_out[i] = vv.vega;
  }
  return Ok(n);
}

Result<std::size_t> implied_vol_batch(std::span<const double> price, std::span<const double> F,
                                      std::span<const double> K, std::span<const double> T,
                                      std::span<const double> df, std::span<const Side> side,
                                      std::span<double> iv_out, std::span<Status> status_out) {
  if (!sizes_match({price.size(), F.size(), K.size(), T.size(), df.size(), side.size(),
                    iv_out.size(), status_out.size()})) {
    return Err(ErrorCode::InvalidArgument, "implied_vol_batch: span length mismatch");
  }
  if (any_input_alias_conflict(iv_out, price, F, K, T, df, side) ||
      any_input_alias_conflict(status_out, price, F, K, T, df, side) ||
      spans_overlap(iv_out, status_out)) {
    return Err(ErrorCode::InvalidArgument,
               "implied_vol_batch: outputs overlap each other or partially overlap an input (only "
               "exact in-place aliasing is permitted)");
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
  return Ok(n);
}

Result<std::size_t> black76_greeks_batch(std::span<const double> F, std::span<const double> K,
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
  if (any_input_alias_conflict(greeks_out, F, K, T, sigma, r, df, side) ||
      any_input_alias_conflict(price_out, F, K, T, sigma, r, df, side) ||
      spans_overlap(greeks_out, price_out)) {
    return Err(ErrorCode::InvalidArgument,
               "black76_greeks_batch: outputs overlap each other or partially overlap an input "
               "(only exact in-place aliasing is permitted)");
  }
  const std::size_t n = F.size();
  if (n >= kAvx2LaneWidth && simd::use_avx2()) {
    simd::detail::black76_greeks_batch_avx2(F.data(), K.data(), T.data(), sigma.data(), r.data(),
                                            df.data(), side.data(), greeks_out.data(),
                                            want_price ? price_out.data() : nullptr, n);
    return Ok(n);
  }
  for (std::size_t i = 0; i < n; ++i) {
    const Black76Greeks g = black76_greeks(F[i], K[i], T[i], sigma[i], r[i], df[i], side[i]);
    greeks_out[i] = g.greeks;
    if (want_price) {
      price_out[i] = g.price;
    }
  }
  return Ok(n);
}

Result<std::size_t> essvi_w_batch(const EssviSlice &slice, std::span<const double> k_log,
                                  std::span<double> w_out) {
  if (k_log.size() != w_out.size()) {
    return Err(ErrorCode::InvalidArgument, "essvi_w_batch: span length mismatch");
  }
  if (input_alias_conflict(w_out, k_log)) {
    return Err(ErrorCode::InvalidArgument,
               "essvi_w_batch: output overlaps the input (only exact in-place aliasing is "
               "permitted)");
  }
  const std::size_t n = k_log.size();
  if (n >= kEssviAvx2MinBatch && simd::use_avx2()) {
    EssviParams params{};
    params.theta = slice.theta;
    params.phi = slice.phi;
    params.rho = slice.rho;
    params.rho_R = slice.rho;
    params.T = slice.T;
    simd::detail::essvi_backbone_w_batch_avx2(params, k_log.data(), w_out.data(), n);
    return Ok(n);
  }
  for (std::size_t i = 0; i < n; ++i) {
    w_out[i] = essvi_w(slice, k_log[i]);
  }
  return Ok(n);
}

} // namespace atx::vol
