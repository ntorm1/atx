#pragma once

// ── Per-expiry PCP borrow (carry) memo ──────────────────────────────────────
//
// WHY THIS EXISTS. `resolve_chain_forward`'s American route solves the put-call
// parity borrow fixed point: borrow -> F -> q_eff -> de-Americanized leg vols ->
// European mids -> borrow, iterated to |Δborrow| < 1e-8 over up to
// `max_borrow_pairs` co-terminal near-ATM pairs. Each iteration costs TWO full
// American implied-vol inversions, and the loop measures 7.2 iterations per pair,
// so one expiry's carry costs ~72 cold Andersen-Lake inversions. Measured with
// the `alprobe` `carry_chain` zone on a 612-board populate date (2025-09-11):
// **50.8% of all board_fit CPU**, and 79.6% of every scalar IV inversion the
// fitter runs. It is the single largest block in the surface fit — larger than
// the per-strike de-Americanization it is usually mistaken for.
//
// And it was being recomputed. The carry is a property of the (symbol, expiry)
// board state, but nothing memoized it, so the SAME fixed point was re-solved by
// the curve selector, by every fallback-ladder rung's surface build, by the
// market-mark build, by the certification pass and by the eSSVI parity lane —
// each of them calling `resolve_chain_forward` on an identical chain with
// identical inputs.
//
// WHAT MAKES THIS EXACT rather than an approximation. The solve is a pure,
// deterministic function of its inputs: same bits in, same bits out. The memo
// therefore returns precisely what a re-solve would return, so the fitted
// surface is bit-identical with the memo on or off — which is the gate this was
// built to pass, and is verified by hashing the whole session's surface
// partition both ways.
//
// KEY DESIGN. `CarryKey` carries every input the solve reads, compared EXACTLY
// (a 64-bit hash only pre-filters buckets; a hit still requires a full
// element-wise compare, so a collision cannot serve a wrong carry). Two
// deliberate reductions, both exact rather than lossy:
//
//   * the dividend schedule, the expiry timestamp and the hybrid-dividend
//     parameters are folded into the single scalar `forward_base`, because
//     `hybrid_forward_base` is the ONLY channel through which they reach the
//     solve. Two dividend states that produce the same base produce the same
//     carry, so folding them cannot serve a stale answer.
//   * the CONFIDENCE knobs (`min_confident_borrow_pairs`, `max_carry_dispersion`,
//     `max_carry_leave_one_out`, `require_carry_confidence`) are NOT in the key
//     and NOT in the value. They are applied by the caller after the lookup, so
//     the curve lane's ungated probe and the risk build's gated resolve share one
//     entry and each keeps its own exact verdict.
//   * `n_atm`, `max_borrow_pairs` and `carry_atm_band` are replaced by
//     `selected_pairs`, the pair COUNT they resolve to. Those three knobs reach
//     the solve only through `select_carry_pairs`' cut
//     `k = min(min(max(n_atm, band_count), max_borrow_pairs), n_valid)`, and the
//     candidate list itself is already pinned by the quote arrays and `S`. So two
//     quality modes that differ in `n_atm` but land on the same `k` are running
//     the identical computation and may share one entry — which they do, because
//     a board dense enough to fill the ±band saturates `max_borrow_pairs`
//     regardless of `n_atm`.
//
// Everything the solve does NOT read is likewise absent: `caches` (the carry
// deliberately runs on empty cold caches), `al_opts`, `iv_tol`, `iv_max_iter`.
//
// Thread-safety: many concurrent readers, exclusive writers, one `shared_mutex`.
// Fit workers fan out per chain, so lookups are ~1 per (expiry, rung) against a
// solve that costs millions of cycles — the lock is never contended in any
// measurable sense. Capacity is a fixed ring (JPL Rule 3: bounded, no unbounded
// growth); the oldest entry is overwritten, never a live one being read.
//
// Library-private: src/-only, NOT installed. `carry_memo_reset` exists so a test
// can start from a known state.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "atx/vol/api/fitting/deamer.hpp" // CarryDiagnostics
#include "atx/vol/api/pricing/american.hpp" // AlOpts, AmericanMethod

namespace atx::vol::carry_memo {

// How a memoized solve ended. The three failure kinds are distinguished so the
// caller reproduces the SAME error the cold path returns, message included.
enum class Outcome : std::uint8_t {
  Ok = 0,
  AllPairsFailed,    // every attempted pair's fixed point failed
  AggregationFailed, // the robust location came back non-finite
};

// Every input the American carry solve reads, in one comparable record. Spans
// are NON-OWNING and must stay valid for the duration of the lookup/store call;
// `store` copies what it keeps.
struct Key {
  double T{0.0};
  double forward_base{0.0}; // folds cash_divs + expiry_ns + hyb (see header)
  double S{0.0};
  double r{0.0};
  std::int64_t now_ts_ns{0};
  AmericanMethod method{AmericanMethod::AndersenLake};
  bool has_carry_al_opts{false};
  AlOpts carry_al_opts{};
  std::size_t selected_pairs{0}; // select_carry_pairs' k — see the header
  bool warm_start_carry{false};
  std::span<const double> strikes;
  std::span<const double> bids;
  std::span<const double> asks;
  std::span<const double> mids;
  std::span<const std::uint8_t> flags;
  std::span<const std::int64_t> ts_ns;
};

// The solve's result at the point just BEFORE the confidence gate: the robust
// borrow and the raw diagnostics. `diag.confident` is left as the solve left it
// (false) — the caller stamps it.
struct Value {
  Outcome outcome{Outcome::Ok};
  double borrow{0.0};
  CarryDiagnostics diag{};
};

// Serve `key` from the memo. Returns false on a miss (and leaves `out` alone).
[[nodiscard]] bool lookup(const Key &key, Value &out);

// Publish a solved carry. A no-op when the memo is disabled or the key is
// already present.
void store(const Key &key, const Value &value);

// Drop every entry. Test hook — production never calls it.
void reset();

// False when `ATX_VOL_CARRY_MEMO=0` disabled the memo at process start. Read
// once at TU init; every hot-path read is a plain const-bool load.
[[nodiscard]] bool enabled() noexcept;

} // namespace atx::vol::carry_memo

namespace atx::vol {

// Convenience alias so a test does not have to name the inner namespace.
inline void carry_memo_reset() { carry_memo::reset(); }

} // namespace atx::vol
