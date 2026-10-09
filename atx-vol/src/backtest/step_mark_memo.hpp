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
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/api/backtest/backtest.hpp"         // Lot, MarketSnapshot
#include "atx/vol/api/backtest/portfolio_pricer.hpp" // PortfolioPricer, PortfolioWorkspace, RetainedMark
#include "atx/vol/api/core/types.hpp"            // Side, PriceStatus

namespace atx::vol::detail {

// A book-greeks pass at a base date POPULATES it (via PortfolioPricer::retained_marks);
// the NEXT step's settlement (same base date, the expiring lot still priced by that
// pass) READS the lot's base mark instead of re-solving it. Keyed by bit-exact
// (uid,K,T,side) and validated against the uid's base-surface instance id, so a
// stale/mismatched entry fails closed to a fresh solve. Reset+repopulated on every
// populated step (holds ONE date). The served mark is an ECONOMIC PARITY match to
// the settlement solve, not a bit-identity one: FullGreeks mark and Marks mark for
// the same contract agree to <=1e-10 relative (~1e-13 USD) -- the L2 crux gate
// (`L2MarkMemoCruxFullGreeksMarkEqualsMarksMark`, backtest_exec_test.cpp), which
// also documents WHY the two routes can diverge at all (an AVX2 batch-composition
// reassociation residual between the FullGreeks batch and the Marks-only batch,
// not a model split). `L2StrategyCohortSettlementMemoBitIdentical` in the same file
// measures that residual propagated through a multi-settlement run (~1e-14
// relative to the settlement mark itself, ~1e-11 absolute accumulated into NAV).
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
    begin_generation(marks.size());
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
      insert(key_of(m.uid, m.K, m.T, m.side), Val{inst, m.mark});
    }
  }

  [[nodiscard]] std::optional<double> find(std::uint32_t uid, double K, double T, Side side,
                                           std::uint64_t base_surface_instance) const {
    const Slot *slot = probe(key_of(uid, K, T, side));
    if (slot == nullptr || slot->val.instance != base_surface_instance) {
      return std::nullopt;
    }
    return slot->val.mark;
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
  struct Val {
    std::uint64_t instance;
    double mark;
  };
  // 6.6: one dense slot vector, stamped with the populate generation, replaces the
  // node-based `unordered_map` this used to be. `clear()` + `reserve()` on that map
  // FREED every node each step and heap-allocated one per admitted mark on the next
  // — an allocate/free pair per lot per step, on the step loop. Bumping a counter
  // retires the whole table instead, so a warm memo is allocation-free: the slot
  // vector is grow-only and steady-state populate touches nothing but slots whose
  // stamp is already stale.
  struct Slot {
    std::uint64_t gen{0u}; // 0 == never written; `generation_` starts at 1
    Key key{};
    Val val{};
  };
  [[nodiscard]] static std::uint64_t hash_of(const Key &k) noexcept {
    std::uint64_t h = 0x9e3779b97f4a7c15ULL ^ static_cast<std::uint64_t>(k.uid);
    const auto mix = [&h](std::uint64_t v) {
      h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    };
    mix(k.kbits);
    mix(k.tbits);
    mix(static_cast<std::uint64_t>(k.side));
    // splitmix64 finalizer: `mix` alone leaves the low bits of a strike/tenor pair
    // poorly diffused, and the bucket index is exactly those low bits.
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return h;
  }
  [[nodiscard]] static Key key_of(std::uint32_t uid, double K, double T, Side side) noexcept {
    return Key{uid, std::bit_cast<std::uint64_t>(K), std::bit_cast<std::uint64_t>(T),
               static_cast<std::uint8_t>(side)};
  }

  // Retire every live entry in O(1) and size the table for `n` admissions. Capacity is
  // a power of two >= 2n (max load factor 0.5, so linear probing stays short) and
  // never shrinks. Growing MUST also retire the old contents, which it does for free:
  // a fresh vector's slots are all gen 0 != generation_.
  void begin_generation(std::size_t n) {
    ++generation_;
    std::size_t want = 8u;
    while (want < 2u * n) {
      want *= 2u;
    }
    if (table_.size() < want) {
      table_.assign(want, Slot{});
    }
    mask_ = table_.size() - 1u;
  }
  // Last write wins on a duplicate key, exactly as `entries_[k] = v` did.
  void insert(const Key &k, const Val &v) {
    std::size_t i = static_cast<std::size_t>(hash_of(k)) & mask_;
    for (;;) {
      Slot &s = table_[i];
      if (s.gen != generation_ || s.key == k) {
        s.gen = generation_;
        s.key = k;
        s.val = v;
        return;
      }
      i = (i + 1u) & mask_;
    }
  }
  [[nodiscard]] const Slot *probe(const Key &k) const noexcept {
    if (table_.empty()) {
      return nullptr;
    }
    std::size_t i = static_cast<std::size_t>(hash_of(k)) & mask_;
    for (;;) {
      const Slot &s = table_[i];
      if (s.gen != generation_) {
        return nullptr; // stale/never-written slot terminates the probe run
      }
      if (s.key == k) {
        return &s;
      }
      i = (i + 1u) & mask_;
    }
  }

  std::vector<Slot> table_;
  std::size_t mask_{0u};
  std::uint64_t generation_{0u};
  std::vector<RetainedMark> marks_scratch_;
  std::vector<Lot> solve_lots_;
  std::vector<double> served_;
};

} // namespace atx::vol::detail
