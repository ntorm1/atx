#pragma once

// L2 (AL-solve-wall sprint) settlement mark memo — the per-step
// per-(unique-contract, base-surface) mark cache the backtest step loop populates
// from a book-greeks pass and reads on the NEXT step's settlement.
//
// WHY THIS IS A HEADER AND NOT backtest.cpp's anonymous namespace. The memo's
// ADMISSION CONTRACT (which marks may be served in place of a solve) is the whole
// safety argument for the optimization, and the marks that would violate it cannot
// be manufactured through the public pricing stack — every Ok-stamp in
// portfolio_pricer.cpp sweeps them upstream. Testing the contract therefore needs
// the same direct seam `src/laned_greek_run.hpp` gives the Greek Ok-stamp: a
// src-private header, included by `src/backtest.cpp` and by its test. Not part of
// the installed public API.

#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "atx/vol/backtest.hpp"         // Lot, MarketSnapshot
#include "atx/vol/portfolio_pricer.hpp" // PortfolioPricer, PortfolioWorkspace, RetainedMark
#include "atx/vol/types.hpp"            // Side, PriceStatus

namespace atx::vol::detail {

// A book-greeks pass at a base date POPULATES it (via PortfolioPricer::retained_marks);
// the NEXT step's settlement (same base date, the expiring lot still priced by that
// pass) READS the lot's base mark instead of re-solving it. Keyed by bit-exact
// (uid,K,T,side) and validated against the uid's base-surface instance id, so a
// stale/mismatched entry fails closed to a fresh solve. Reset+repopulated on every
// populated step (holds ONE date). The served mark is bit-identical to the
// settlement solve (FullGreeks mark == Marks mark, L2 crux gate).
class StepMarkMemo {
public:
  void populate_from(const PortfolioPricer &pricer, const PortfolioWorkspace &ws,
                     const MarketSnapshot &snap) {
    pricer.retained_marks(ws, marks_scratch_);
    populate_from_marks(marks_scratch_, snap.set());
  }

  // The admission gate itself, taking the marks as data so a test can drive it with
  // a mark the live pricer will not produce (see the file header).
  void populate_from_marks(std::span<const RetainedMark> marks, const SurfaceSet &surfaces) {
    entries_.clear();
    entries_.reserve(marks.size());
    for (const RetainedMark &m : marks) {
      if (m.status != PriceStatus::Ok) {
        continue; // only Ok marks are servable; a failed one must re-solve / fail closed
      }
      // Plan 1.11: NaN is the settlement path's IN-BAND "this lot must be solved"
      // sentinel (`served_scratch` seeds it, `!isnan(served[i])` reads it), so a
      // non-finite mark admitted here is served AND reads back as a miss — a null
      // solve-frame dereference when it is the only expiring lot, a desynced
      // `solve_ix` when it is not. Admit only finite marks: a non-finite Ok mark
      // becomes an ordinary memo MISS and falls through to the normal solve, which
      // re-derives it under the pricer's own finite Ok-stamp. Costs nothing on the
      // healthy path — every mark a live solve stamps Ok is already finite.
      if (!std::isfinite(m.mark)) {
        continue;
      }
      const SurfaceRef s = surfaces.find(m.uid);
      const std::uint64_t inst = s != nullptr ? s->instance_id() : 0u;
      entries_[key_of(m.uid, m.K, m.T, m.side)] = Val{inst, m.mark};
    }
  }

  [[nodiscard]] std::optional<double> find(std::uint32_t uid, double K, double T, Side side,
                                           std::uint64_t base_surface_instance) const {
    const auto it = entries_.find(key_of(uid, K, T, side));
    if (it == entries_.end() || it->second.instance != base_surface_instance) {
      return std::nullopt;
    }
    return it->second.mark;
  }

  // Reusable settlement scratch (grow-only; keeps compute_step allocation-free after
  // warm even on settlement steps).
  [[nodiscard]] std::vector<Lot> &solve_scratch() {
    solve_lots_.clear();
    return solve_lots_;
  }
  [[nodiscard]] std::vector<double> &served_scratch(std::size_t n) {
    served_.assign(n, std::numeric_limits<double>::quiet_NaN());
    return served_;
  }

private:
  struct Key {
    std::uint32_t uid;
    std::uint64_t kbits;
    std::uint64_t tbits;
    std::uint8_t side;
    bool operator==(const Key &) const noexcept = default;
  };
  struct KeyHash {
    [[nodiscard]] std::size_t operator()(const Key &k) const noexcept {
      std::size_t h = std::hash<std::uint32_t>{}(k.uid);
      const auto mix = [&h](std::uint64_t v) {
        h ^= std::hash<std::uint64_t>{}(v) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      };
      mix(k.kbits);
      mix(k.tbits);
      mix(static_cast<std::uint64_t>(k.side));
      return h;
    }
  };
  struct Val {
    std::uint64_t instance;
    double mark;
  };
  [[nodiscard]] static Key key_of(std::uint32_t uid, double K, double T, Side side) noexcept {
    return Key{uid, std::bit_cast<std::uint64_t>(K), std::bit_cast<std::uint64_t>(T),
               static_cast<std::uint8_t>(side)};
  }

  std::unordered_map<Key, Val, KeyHash> entries_;
  std::vector<RetainedMark> marks_scratch_;
  std::vector<Lot> solve_lots_;
  std::vector<double> served_;
};

} // namespace atx::vol::detail
