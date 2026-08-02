// PreparedPortfolio implementation — build the stable, aligned, grouped execution
// substrate over a Portfolio's unique contracts. See prepared_portfolio.hpp.

#include "atx/vol/prepared_portfolio.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <numeric>

namespace atx::vol {

namespace {

// Total-order key on a double's raw bits (the classic radix/IEEE-total-order
// transform): finite values sort in ascending numeric order, and ANY bit pattern
// (including a non-finite/degenerate contract T the Portfolio may carry) maps to a
// distinct, comparable key. Using this instead of `a.T < b.T` keeps the sort
// comparator a STRICT WEAK ORDERING even when a degenerate contract's T is NaN
// (`<` on NaN is incomparable — undefined behaviour for std::sort). Because the
// map is a bijection on bit patterns, `key(a) == key(b)` iff a and b are
// RAW-BIT-EQUAL — exactly the grouping the T-bracket ladder reuse needs (no
// tolerance, no quantization).
[[nodiscard]] std::uint64_t total_order_key(double x) noexcept {
  const std::uint64_t b = std::bit_cast<std::uint64_t>(x);
  const std::uint64_t mask =
      (b & 0x8000000000000000ULL) ? 0xFFFFFFFFFFFFFFFFULL : 0x8000000000000000ULL;
  return b ^ mask;
}

}  // namespace

Result<PreparedPortfolio> PreparedPortfolio::create(const Portfolio& pf, const PriceOptions& opts) {
  (void)opts; // Route/method/ISA are call or surface state, never persisted book state.
  const std::span<const OptionContract> contracts = pf.contracts();
  const std::size_t n = contracts.size();

  PreparedPortfolio pp;
  pp.n_ = n;
  pp.k_ = detail::AlignedColumn<double>(n);
  pp.t_ = detail::AlignedColumn<double>(n);
  pp.uid_ = detail::AlignedColumn<std::uint32_t>(n);
  if (n != 0 &&
      (pp.k_.data() == nullptr || pp.t_.data() == nullptr || pp.uid_.data() == nullptr)) {
    return Err(ErrorCode::Internal, "PreparedPortfolio::create: aligned column allocation failed");
  }
  pp.side_.resize(n);
  pp.oci_.resize(n);

  // 1. Compute the execution permutation over the UNIQUE contracts. We sort an
  //    index vector, NOT the Portfolio's contract table — the Portfolio (and every
  //    position's contract_ix into it) is left completely untouched.
  //
  //    Key: (uid, side, T) as a STRICT TOTAL ORDER, tie-broken by the original
  //    contract index. A stable sort would already keep first-appearance order for
  //    equal keys, but making the comparator itself a total order (the final
  //    `a < b`) removes any ambiguity — the permutation is deterministic
  //    independent of the sort's stability. Sorting by (uid, side, T) makes each
  //    (uid, side) run CONTIGUOUS (SIMD-homogeneous) with its members in
  //    ascending-T order, so every raw-bit-equal-T sub-run is contiguous (an
  //    evaluate_batch ladder). We NEVER quantize any key.
  std::vector<std::uint32_t> perm(n);
  std::iota(perm.begin(), perm.end(), 0u);
  std::stable_sort(perm.begin(), perm.end(), [&](std::uint32_t a, std::uint32_t b) {
    const OptionContract& ca = contracts[a];
    const OptionContract& cb = contracts[b];
    if (ca.uid != cb.uid) {
      return ca.uid < cb.uid;
    }
    const auto sa = static_cast<std::uint8_t>(ca.side);
    const auto sb = static_cast<std::uint8_t>(cb.side);
    if (sa != sb) {
      return sa < sb;
    }
    const std::uint64_t ta = total_order_key(ca.T);
    const std::uint64_t tb = total_order_key(cb.T);
    if (ta != tb) {
      return ta < tb;
    }
    return a < b;  // original contract index: the final, total-order tie-break
  });

  // 2. Materialize the aligned SoA in permuted order and the reverse permutation.
  //    original_contract_index()[p] == perm[p] is the Portfolio contract index for
  //    permuted slot p, so a grouped solve writes back into the exact slot the
  //    position scatter reads.
  for (std::size_t p = 0; p < n; ++p) {
    const std::uint32_t orig = perm[p];
    const OptionContract& c = contracts[orig];
    pp.k_.data()[p] = c.K;
    pp.t_.data()[p] = c.T;
    pp.uid_.data()[p] = c.uid;
    pp.side_[p] = c.side;
    pp.oci_[p] = orig;
  }

  // 3. Partition the permuted order into maximal (uid, side) runs. Contiguous by
  //    construction (step 1 sorted (uid, side) adjacent), so the groups tile
  //    [0, n_unique) with no gap or overlap.
  std::size_t p = 0;
  while (p < n) {
    const std::uint32_t u = pp.uid_.data()[p];
    const Side s = pp.side_[p];
    std::size_t q = p + 1;
    while (q < n && pp.uid_.data()[q] == u && pp.side_[q] == s) {
      ++q;
    }
    pp.groups_.push_back(ContractGroup{u, s, static_cast<std::uint32_t>(p),
                                       static_cast<std::uint32_t>(q), GroupRoute{}});

    // FullGreeks work is deliberately tiled only by the enclosing (uid, side)
    // group, not by raw T. The fixed width gives run_dynamic enough immutable
    // work units to balance heterogeneous solves while every worker still owns
    // disjoint permuted slots. evaluate_batch retains its internal ascending-T
    // run handling inside each tile.
    std::size_t greek_tile_begin = p;
    while (greek_tile_begin < q) {
      const std::size_t greek_tile_end =
          std::min(greek_tile_begin + static_cast<std::size_t>(kPreparedGreekTileLanes), q);
      pp.greek_tiles_.push_back(PreparedGreekTile{u, s,
                                                  static_cast<std::uint32_t>(greek_tile_begin),
                                                  static_cast<std::uint32_t>(greek_tile_end)});
      greek_tile_begin = greek_tile_end;
    }

    // Subdivide each raw-bit-identical expiry run into fixed, AVX-width-aligned
    // tiles. The immutable book alone determines these boundaries, so changing
    // worker count cannot move a lane between a complete pack and scalar tail.
    std::size_t run_begin = p;
    while (run_begin < q) {
      const std::uint64_t t_bits = std::bit_cast<std::uint64_t>(pp.t_.data()[run_begin]);
      std::size_t run_end = run_begin + 1;
      while (run_end < q &&
             std::bit_cast<std::uint64_t>(pp.t_.data()[run_end]) == t_bits) {
        ++run_end;
      }
      std::size_t tile_begin = run_begin;
      while (tile_begin < run_end) {
        const std::size_t tile_end =
            std::min(tile_begin + static_cast<std::size_t>(kPreparedPriceTileLanes), run_end);
        pp.price_tiles_.push_back(
            PreparedPriceTile{u, s, t_bits, static_cast<std::uint32_t>(tile_begin),
                              static_cast<std::uint32_t>(tile_end)});
        tile_begin = tile_end;
      }
      run_begin = run_end;
    }
    p = q;
  }

  return pp;
}

bool PreparedPortfolio::try_refresh_tenors(const Portfolio& pf) noexcept {
  const std::span<const OptionContract> contracts = pf.contracts();
  if (contracts.size() != n_ || k_.size() != n_ || t_.size() != n_ || uid_.size() != n_ ||
      side_.size() != n_ || oci_.size() != n_) {
    return false;
  }

  for (std::size_t p = 0; p < n_; ++p) {
    const std::uint32_t orig = oci_[p];
    if (orig >= contracts.size()) {
      return false;
    }
    const OptionContract& c = contracts[orig];
    if (c.uid != uid_.data()[p] || c.side != side_[p] ||
        std::bit_cast<std::uint64_t>(c.K) != std::bit_cast<std::uint64_t>(k_.data()[p])) {
      return false;
    }
    if (p == 0) {
      continue;
    }
    const std::uint32_t prev_orig = oci_[p - 1u];
    const OptionContract& prev = contracts[prev_orig];
    const bool same_group = prev.uid == c.uid && prev.side == c.side;
    if (same_group) {
      const std::uint64_t prev_key = total_order_key(prev.T);
      const std::uint64_t cur_key = total_order_key(c.T);
      if (prev_key > cur_key || (prev_key == cur_key && prev_orig > orig)) {
        return false;
      }
      const bool old_equal =
          std::bit_cast<std::uint64_t>(t_.data()[p - 1u]) ==
          std::bit_cast<std::uint64_t>(t_.data()[p]);
      const bool next_equal = std::bit_cast<std::uint64_t>(prev.T) ==
                              std::bit_cast<std::uint64_t>(c.T);
      if (old_equal != next_equal) {
        return false;
      }
    }
  }

  std::size_t covered = 0;
  for (const PreparedPriceTile& tile : price_tiles_) {
    if (tile.begin != covered || tile.begin >= tile.end || tile.end > n_) {
      return false;
    }
    for (std::size_t p = tile.begin; p < tile.end; ++p) {
      if (uid_.data()[p] != tile.uid || side_[p] != tile.side ||
          std::bit_cast<std::uint64_t>(t_.data()[p]) != tile.t_bits) {
        return false;
      }
    }
    covered = tile.end;
  }
  if (covered != n_) {
    return false;
  }

  for (std::size_t p = 0; p < n_; ++p) {
    t_.data()[p] = contracts[oci_[p]].T;
  }
  for (PreparedPriceTile& tile : price_tiles_) {
    tile.t_bits = std::bit_cast<std::uint64_t>(t_.data()[tile.begin]);
  }
  return true;
}

}  // namespace atx::vol
