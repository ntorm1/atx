#pragma once

// PreparedPortfolio — a stable, aligned, GROUPED execution view over a
// Portfolio's UNIQUE contracts.
//
// ## Why this exists
//
// `PortfolioPricer` used to iterate the unique contracts in first-appearance
// insertion order (`Portfolio::create` dedups into `contracts_` in the order the
// positions arrive) with no expiry grouping and no bracket reuse. Two later
// packages need a grouped, aligned substrate:
//
//   * P2.5 / `PricedSurface::evaluate_batch`: a run of contracts sharing
//     `(uid, expiry)` resolves the T-bracket + carry ONCE and reuses it across the
//     strike ladder (bit-identical because T is compared by raw bits, never a
//     tolerance, and `interp_forward` is monotone in T).
//   * P3.2 AVX2: four independent contracts of the same side/scheme/route are
//     packed into one AoSoA<4> lane block; that packing needs contiguous,
//     homogeneous groups.
//
// `PreparedPortfolio` is that substrate.
//
// ## The bit-identity contract (the whole point)
//
// Positions and their INPUT ORDER are untouched — this type never sorts positions.
// Only the *unique-contract execution order* is permuted (grouped by `(uid, side)`,
// ascending-T ladders inside), and `original_contract_index()` is the REVERSE
// permutation back to the Portfolio's contract index. A grouped solve therefore
// writes each unique result into the SAME `px[...]` slot the existing position
// scatter reads via `Portfolio::contract_ix(i)`, so the downstream scatter and the
// fixed-order totals reduction are byte-for-byte unchanged. Permuting a set of
// DISJOINT-slot writes cannot change any output; the permutation only reorders when
// each unique contract is priced, and (via the reverse permutation) where the
// result lands is invariant.
//
// ## No sigma, ever
//
// A `PreparedPortfolio` is built BEFORE any surface is resolved (the surface is a
// runtime input to `price()`), so it carries no sigma and no surface state. Its
// grouping key is exact contract metadata `(uid, side, T)` only — it NEVER
// quantizes K, T, or sigma to manufacture a shared group. Members of a group keep
// their exact distinct `(K, T)`.
//
// ## Thread-safety
//
// Immutable after `create`; all accessors are const reads of value state.

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

#include "atx/core/aligned.hpp"          // aligned_alloc_bytes / aligned_free / kCacheLineSize
#include "atx/vol/portfolio_pricer.hpp"  // Portfolio, PriceOptions, OptionContract
#include "atx/vol/types.hpp"             // Result, Side

namespace atx::vol {

namespace detail {

// Owning, cache-line-aligned (64-byte) contiguous column of a trivially-copyable
// element type. A single bounded allocation per column (never per element). This
// is the staging buffer an AVX2 load (T13) needs: `data()` is 64-byte aligned,
// which is a multiple of AVX2's 32-byte requirement. Move-only (owns the aligned
// block); a moved-from column is empty.
template <class T>
class AlignedColumn {
  static_assert(std::is_trivially_copyable_v<T>, "AlignedColumn: T must be trivially copyable");

 public:
  AlignedColumn() = default;

  // Allocate `n` elements 64-byte aligned. On allocation failure (n > 0) `data()`
  // is nullptr — the caller (create) checks and surfaces an error.
  explicit AlignedColumn(std::size_t n) : n_{n} {
    if (n_ != 0) {
      data_ = static_cast<T*>(
          atx::core::aligned_alloc_bytes(n_ * sizeof(T), atx::core::kCacheLineSize));
    }
  }

  ~AlignedColumn() { atx::core::aligned_free(data_); }

  AlignedColumn(AlignedColumn&& o) noexcept : data_{o.data_}, n_{o.n_} {
    o.data_ = nullptr;
    o.n_ = 0;
  }
  AlignedColumn& operator=(AlignedColumn&& o) noexcept {
    if (this != &o) {
      atx::core::aligned_free(data_);
      data_ = o.data_;
      n_ = o.n_;
      o.data_ = nullptr;
      o.n_ = 0;
    }
    return *this;
  }
  AlignedColumn(const AlignedColumn&) = delete;
  AlignedColumn& operator=(const AlignedColumn&) = delete;

  [[nodiscard]] T* data() noexcept { return data_; }
  [[nodiscard]] const T* data() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return n_; }
  [[nodiscard]] std::span<const T> span() const noexcept { return {data_, n_}; }

 private:
  T* data_{nullptr};
  std::size_t n_{0};
};

}  // namespace detail

// The scheme/route identity a group prices under — what makes its members
// homogeneous for a later AVX2 pack (T13). At prepare time no surface is resolved,
// so the concrete `AlOpts`/`AmericanMethod` are not yet known; the route a group
// WILL price under is pinned by (a) its shared `uid` — one `PricedSurface`, hence
// one method/AlOpts — and (b) the book-wide `PriceOptions` Greek scheme. This POD
// records the latter so a packer can assert lane homogeneity without re-deriving
// it. Small + trivially copyable by design.
struct GroupRoute {
  bool analytic_greeks{false};  // american_greeks_al vs american_greeks_fd Greek route
  bool prices_only{false};      // price-only (Iv + mark, no Greeks) route
};

// A maximal contiguous run of the permuted unique-contract order that shares a
// `(uid, side)` — homogeneous for SIMD packing. `[begin, end)` indexes the PERMUTED
// unique-contract order (the SoA columns below). Within a group, entries are in
// ascending-T order, so any raw-bit-equal-T sub-run is contiguous (an
// `evaluate_batch` ladder).
struct ContractGroup {
  std::uint32_t uid;
  Side side;
  std::uint32_t begin;  // [begin, end) into the PERMUTED unique-contract order
  std::uint32_t end;
  GroupRoute route;
};

class PreparedPortfolio {
 public:
  // Build the grouped, aligned substrate from a Portfolio's already-deduped unique
  // contracts. Consumes `pf.contracts()` verbatim — never re-dedups or re-hashes.
  // `opts` supplies only the book-wide route (Greek scheme) stamped onto each
  // group; it does not affect the permutation. @return Internal only if an aligned
  // column allocation fails (never on a well-formed book).
  [[nodiscard]] static Result<PreparedPortfolio> create(const Portfolio& pf,
                                                        const PriceOptions& opts);

  // Move-only: owns aligned column allocations.
  PreparedPortfolio(PreparedPortfolio&&) noexcept = default;
  PreparedPortfolio& operator=(PreparedPortfolio&&) noexcept = default;
  PreparedPortfolio(const PreparedPortfolio&) = delete;
  PreparedPortfolio& operator=(const PreparedPortfolio&) = delete;

  // ── Aligned SoA over the unique contracts, in PERMUTED execution order ────────
  [[nodiscard]] std::span<const double> k() const noexcept { return {k_.data(), n_}; }
  [[nodiscard]] std::span<const double> t() const noexcept { return {t_.data(), n_}; }
  [[nodiscard]] std::span<const Side> side() const noexcept { return side_; }
  [[nodiscard]] std::span<const std::uint32_t> uid() const noexcept { return {uid_.data(), n_}; }

  // Reverse permutation: permuted slot `p` -> the ORIGINAL Portfolio contract index
  // its result belongs in (for the scatter/gather). A true permutation of
  // `[0, n_unique)`.
  [[nodiscard]] std::span<const std::uint32_t> original_contract_index() const noexcept {
    return oci_;
  }

  [[nodiscard]] std::span<const ContractGroup> groups() const noexcept { return groups_; }
  [[nodiscard]] std::size_t n_unique() const noexcept { return n_; }

 private:
  PreparedPortfolio() = default;

  std::size_t n_{0};
  detail::AlignedColumn<double> k_;         // strike, 64-byte aligned
  detail::AlignedColumn<double> t_;         // year-fraction, 64-byte aligned
  detail::AlignedColumn<std::uint32_t> uid_;  // underlying id, 64-byte aligned
  std::vector<Side> side_;                  // contiguous (not over-aligned)
  std::vector<std::uint32_t> oci_;          // permuted slot -> original contract index
  std::vector<ContractGroup> groups_;       // partition of [0, n_unique)
};

}  // namespace atx::vol
