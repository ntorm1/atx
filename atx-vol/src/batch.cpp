// PORT NOTE: Scalar-backed batch kernels — numerically identical to the ported
// scalar source of truth; the C's hand-written AVX2 vectorization is a
// documented performance follow-on. Correctness contract (batch == scalar) is
// met exactly.
//
// Each batch entry below is a bounded loop over the already-ported scalar
// kernel (black76_price / black76_price_from_lnfk / black76_value_and_vega /
// implied_vol / black76_greeks / essvi_w). Because every lane invokes the SAME
// function the scalar reference does, the batch output is bit-identical to the
// scalar output on every lane — the exact contract test_pricer_simd.c asserted
// and the C's AVX2 kernels went to lane-patching lengths to preserve. SIMD
// vectorization (AVX2/AVX-512 lane-parallel Chebyshev-Φ + vectorized log/exp,
// with scalar wing/degenerate patch-through) is deferred; it is a throughput
// optimization only and cannot change these numerical results.

#include "atx/vol/batch.hpp"

#include <cstddef>
#include <initializer_list>
#include <limits>

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

}  // namespace

Status black76_price_batch(std::span<const double> F, std::span<const double> K,
                           std::span<const double> T,
                           std::span<const double> sigma,
                           std::span<const double> df,
                           std::span<const Side> side,
                           std::span<double> price_out) {
  if (!sizes_match({F.size(), K.size(), T.size(), sigma.size(), df.size(),
                    side.size(), price_out.size()})) {
    return Err(ErrorCode::InvalidArgument,
               "black76_price_batch: span length mismatch");
  }
  const std::size_t n = F.size();
  for (std::size_t i = 0; i < n; ++i) {
    price_out[i] =
        black76_price(F[i], K[i], T[i], sigma[i], df[i], side[i]);
  }
  return Ok();
}

Status black76_price_from_lnfk_batch(std::span<const double> F,
                                     std::span<const double> K, double T,
                                     double sqrt_t, std::span<const double> sigma,
                                     double df, std::span<const double> ln_fk,
                                     std::span<const Side> side,
                                     std::span<double> price_out) {
  if (!sizes_match({F.size(), K.size(), sigma.size(), ln_fk.size(), side.size(),
                    price_out.size()})) {
    return Err(ErrorCode::InvalidArgument,
               "black76_price_from_lnfk_batch: span length mismatch");
  }
  const std::size_t n = F.size();
  for (std::size_t i = 0; i < n; ++i) {
    price_out[i] = black76_price_from_lnfk(F[i], K[i], T, sigma[i], df,
                                           ln_fk[i], sqrt_t, side[i]);
  }
  return Ok();
}

Status black76_value_and_vega_batch(std::span<const double> F,
                                    std::span<const double> K, double T,
                                    std::span<const double> sigma,
                                    std::span<const double> df,
                                    std::span<const Side> side,
                                    std::span<double> value_out,
                                    std::span<double> vega_out,
                                    double sqrt_t_in) {
  if (!sizes_match({F.size(), K.size(), sigma.size(), df.size(), side.size(),
                    value_out.size(), vega_out.size()})) {
    return Err(ErrorCode::InvalidArgument,
               "black76_value_and_vega_batch: span length mismatch");
  }
  const std::size_t n = F.size();
  for (std::size_t i = 0; i < n; ++i) {
    const Black76ValueVega vv = black76_value_and_vega(
        F[i], K[i], T, sigma[i], df[i], side[i], sqrt_t_in);
    value_out[i] = vv.price;
    vega_out[i] = vv.vega;
  }
  return Ok();
}

Status implied_vol_batch(std::span<const double> price,
                         std::span<const double> F, std::span<const double> K,
                         std::span<const double> T, std::span<const double> df,
                         std::span<const Side> side, std::span<double> iv_out,
                         std::span<Status> status_out) {
  if (!sizes_match({price.size(), F.size(), K.size(), T.size(), df.size(),
                    side.size(), iv_out.size(), status_out.size()})) {
    return Err(ErrorCode::InvalidArgument,
               "implied_vol_batch: span length mismatch");
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

Status black76_greeks_batch(std::span<const double> F,
                            std::span<const double> K, std::span<const double> T,
                            std::span<const double> sigma,
                            std::span<const double> r, std::span<const double> df,
                            std::span<const Side> side,
                            std::span<Greeks> greeks_out,
                            std::span<double> price_out) {
  if (!sizes_match({F.size(), K.size(), T.size(), sigma.size(), r.size(),
                    df.size(), side.size(), greeks_out.size()})) {
    return Err(ErrorCode::InvalidArgument,
               "black76_greeks_batch: span length mismatch");
  }
  const bool want_price = !price_out.empty();
  if (want_price && price_out.size() != F.size()) {
    return Err(ErrorCode::InvalidArgument,
               "black76_greeks_batch: price_out length mismatch");
  }
  const std::size_t n = F.size();
  for (std::size_t i = 0; i < n; ++i) {
    const Black76Greeks g =
        black76_greeks(F[i], K[i], T[i], sigma[i], r[i], df[i], side[i]);
    greeks_out[i] = g.greeks;
    if (want_price) {
      price_out[i] = g.price;
    }
  }
  return Ok();
}

Status essvi_w_batch(const EssviSlice& slice, std::span<const double> k_log,
                     std::span<double> w_out) {
  if (k_log.size() != w_out.size()) {
    return Err(ErrorCode::InvalidArgument,
               "essvi_w_batch: span length mismatch");
  }
  const std::size_t n = k_log.size();
  for (std::size_t i = 0; i < n; ++i) {
    w_out[i] = essvi_w(slice, k_log[i]);
  }
  return Ok();
}

}  // namespace atx::vol
