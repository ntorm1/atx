#pragma once

// K2 (r5, 2026-10-22): the 4-lane AVX2 evaluation of K1's fused (σ, q) family price + vega
// from a precontracted table, one lane per slot, each slot with its own strike, q tile
// and σ (src/pricing/american_iv_batch.hpp is the only caller; it dispatches here only
// when simd::have_avx2() holds).
//
// THE MAP is price_vega_on's (american_sigma_q_family.cpp): the σ basis and its nodal
// derivative on the table's σ axis, two n_sigma-dots per premium node on the slot's q tile,
// the transform s = √max(y, 0), the log-form premium integrand with its σ-derivative and
// the Black-76 put leg, then the al_put_price_from_boundary clamp order. The arithmetic is
// the same expressions in the same order per lane, with the vector Cody Φ, an exp shared
// by Φ(x) and φ(x) (both read e^{−x²/2}), and multiplications by 1/v and 1/σ where the
// scalar divides — a ROUNDING-CLASS difference from the scalar map (measured by
// IvBatch.Avx2KernelPriceAndVegaMatchTheScalarMap). Each lane's result depends only on its
// own slot (no cross-lane reduction), so a lane's bits do not depend on its neighbours.
//
// The load step is scalar and reads the tile K1's contract_q cut, so the vector kernel and
// the scalar reference read bit-identical tiles.

#include <cstddef>
#include <cstdint>

#include "pricing/american_sigma_q_family.hpp"

namespace atx::vol::simd::iv_batch_avx2 {

inline constexpr std::size_t kLanes = 4;
inline constexpr std::size_t kRows = sigma_q_family_detail::kMaxPremiumNodes + 1;
inline constexpr std::size_t kSigmaNodes = sigma_family_detail::kMaxSigmaNodes;

// The four slots' per-contract state, interleaved by slot (index [..][slot]).
struct alignas(32) Slots {
  double sigma[kLanes] = {};
  double Sp[kLanes] = {};    // the internal put's spot / strike
  double Kp[kLanes] = {};
  double rp[kLanes] = {};    // the internal put's rate / yield
  double qp[kLanes] = {};
  double lsx[kLanes] = {};   // ln(Sp / xmax)
  double lsk[kLanes] = {};   // ln(Sp / Kp) (Strike nodes: B = K)
  double drift[kLanes] = {}; // rp − qp
  double ak[kLanes] = {};    // rp · Kp
  double bs[kLanes] = {};    // qp · Sp
  double F[kLanes] = {};     // the European leg's forward and discount
  double df[kLanes] = {};
  double lfk[kLanes] = {};   // ln(F / Kp)
  double intr[kLanes] = {};  // Kp − Sp
  double xmax[kLanes] = {};
  double dq[sigma_q_family_detail::kMaxPremiumNodes][kLanes] = {}; // e^{−qp t_i}
  double dr[sigma_q_family_detail::kMaxPremiumNodes][kLanes] = {}; // e^{−rp t_i}
  double g[kRows * kSigmaNodes][kLanes] = {}; // the q tile's rows, g[i · n_sigma + s][slot]
};

struct Result {
  double price[kLanes] = {};
  double vega[kLanes] = {};
  std::uint8_t exercised[kLanes] = {};
  std::uint8_t degenerate[kLanes] = {};
};

// Slot `k` <- the contract (S, K) on `tile` (cut from `t` by contract_q). Scalar.
void load_slot(const sigma_q_family_detail::PremiumTable &t,
               const sigma_q_family_detail::QTile &tile, double S, double K, std::size_t k,
               Slots &s) noexcept;
// Slot `k` <- the contract (S, K) keeping the slot's tile and carry (same q).
void load_strike(const sigma_q_family_detail::PremiumTable &t, double S, double K, std::size_t k,
                 Slots &s) noexcept;

// Price + vega of all four slots at s.sigma[]. Precondition: every slot was loaded at
// least once from `t` and its σ lies inside the table's box (an unused slot may repeat a
// used one; its result is ignored). Requires AVX2 + FMA at run time.
void price_vega4(const sigma_q_family_detail::PremiumTable &t, const Slots &s,
                 Result &out) noexcept;

} // namespace atx::vol::simd::iv_batch_avx2
