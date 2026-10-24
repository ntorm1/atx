#include "pricing/american_iv_seed_batch.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

#include "american_boundary_avx2_kernel.hpp"
#include "fitting/counters.hpp"

namespace atx::vol::simd::detail {
void iv_seed_reference_pack_avx2(std::span<const IvPriceSeedRequest, 4> requests,
    const std::optional<AlOpts> &opts, std::span<IvSeedVectorPrice, 4> out) noexcept {
  alignas(32) std::array<double, 4> S{}, K{}, T{}, sigma{}, r{}, q{}, price{}, y{}, xmax{};
  for (std::size_t i = 0; i < 4; ++i) {
    out[i] = {};
    const auto &a = requests[i];
    const bool call = a.side == Side::Call;
    S[i] = call ? a.K : a.S;
    K[i] = call ? a.S : a.K;
    T[i] = a.T;
    sigma[i] = a.sigma;
    r[i] = call ? a.q : a.r;
    q[i] = call ? a.r : a.q;
  }
  amer::AlBoundary bnd[4];
  amer::AlWorkspace ws[4];
  PutPackBoundary pack;
  bool eligible[4]{};
  int ref = -1;
  solve_put_boundary_pack_avx2(S.data(), K.data(), T.data(), sigma.data(),
      r.data(), q.data(), 4, amer::scheme_from_opts(opts), bnd, ws, pack, eligible, ref);
  if (ref < 0) return;
  std::array<AlExerciseClass, 4> classes{};
  _mm256_store_pd(price.data(), price_put_pack_at_avx2(pack, _mm256_load_pd(S.data()),
      pack.Kv, pack.XMAX, AlExerciseRegion::KernelFloor, classes.data()));
  _mm256_store_pd(y.data(), pack.nb >= 2 ? pack.Y[pack.nb - 1]
      : _mm256_set1_pd(std::numeric_limits<double>::quiet_NaN()));
  _mm256_store_pd(xmax.data(), pack.XMAX);
  for (std::size_t i = 0; i < 4; ++i) {
    if (!eligible[i]) continue;
    // Same solved endpoint and scalar transform used by the kernel classifier.
    out[i] = {true, price[i], amer::al_boundary_at_expiry(y[i], xmax[i]), classes[i]};
    ATX_VOL_COUNT(BoundarySolves);
    counters::ledger::bump(counters::ledger::Solve::AlBoundarySolves);
  }
}
} // namespace atx::vol::simd::detail
