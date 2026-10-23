#pragma once

// ── Schedule-grouped batch of the D1 production path (lane D1, vol-sota r5,
//    2026-10-22) ──────────────────────────────────────────────────────────────
//
// WHY. Every strike of one (underlier, expiry) shares the spot, the life, the rates
// and the dated cash schedule, and so shares everything a discrete-dividend route
// decides that does not depend on the strike or the volatility: the step count, the
// pre-steps, where each ex-date lands, its forward-preserving cash and its
// re-timing offset (american_discrete_div_kernel.hpp section 4). A batch builds that
// ONCE per group, in `prepare()` — the cold clock's family build + validation stage
// (B0's `family_build_validation`; docs/benchmark-clocks.md) — and every contract of
// the group then runs only its rollbacks, on one workspace, with no heap allocation.
// Nothing is computed before `prepare()` or cached across `prepare()` calls: the
// setup is inside whatever clock times `prepare()`.
//
// GROUPING. The caller states the groups (one per (S, T, r, q, schedule): an expiry
// of an underlier) and tags each contract with its group index; the convention is
// the route's (spot-shift, liquidator, re-timed ex-dates), one per batch. A group
// whose inputs the route or the lattice refuses is a typed per-contract outcome
// (RefusedGroup), never an exception and never a silent price.
//
// Thread-safety: a batch is one thread's (its workspace is); distinct batches are
// independent.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_discrete_div_kernel.hpp"

namespace atx::vol {

// One group: the contracts of one (underlier, expiry).
struct DiscreteDivBatchGroup {
  double S = 0.0;
  double T = 0.0;
  double r = 0.0;
  double q = 0.0;
  // NON-OWNING: the schedule must outlive `prepare()` and every loop after it.
  std::span<const CashDividend> dividends;
};

struct DiscreteDivBatchContract {
  std::uint32_t group = 0; // index into the groups handed to `prepare()`
  double K = 0.0;
  Side side = Side::Call;
  double sigma = 0.0; // `price()`: the volatility priced at
  double quote = 0.0; // `implied_vol()`: the price inverted
};

enum class DiscreteDivBatchOutcome : std::uint8_t {
  Priced = 0,       // a price, or an identified IV root
  PriceOnly = 1,    // implied_vol: the quote sits at or below the lattice's lowest-vol
                    // price — the price is valid, no volatility is identified by it
  RefusedInput = 2, // InvalidArgument: the contract's own inputs (K, sigma, quote)
  RefusedRange = 3, // OutOfRange other than price-only: above the maximum-volatility
                    // price, the CRR window closed, a non-finite lattice price
  RefusedBudget = 4, // Unavailable: the inversion's iteration budget ran out
  RefusedGroup = 5,  // the contract's group failed `prepare()` (or no such group)
};

// Stable names: "priced", "price_only", "refused_input", "refused_range",
// "refused_budget", "refused_group".
[[nodiscard]] const char *to_string(DiscreteDivBatchOutcome value) noexcept;

struct DiscreteDivBatchResult {
  double value = 0.0; // the price (price()) or the root sigma (implied_vol()); NaN if none
  DiscreteDivBatchOutcome outcome = DiscreteDivBatchOutcome::Priced;
  DiscreteDivIvFallback fallback = DiscreteDivIvFallback::None; // implied_vol only
  std::uint16_t screen_lattices = 0;                            // implied_vol only
  std::uint16_t accurate_lattices = 0;                          // implied_vol only
};

// PIMPL (agent profile §6): the per-group routes, step maps and the workspace.
class DiscreteDivBatch {
public:
  // `opts`: the route options of every group (Standard / Refined, base steps, kill
  // switch). `screen_steps`: the Screen tier of `implied_vol()` (a CubicCrr route at
  // that count; 0 disables the Screen tier).
  explicit DiscreteDivBatch(const DiscreteDivProductionOpts &opts = {},
                            int screen_steps = kDiscreteDivScreenSteps);
  ~DiscreteDivBatch();
  DiscreteDivBatch(DiscreteDivBatch &&) noexcept;
  DiscreteDivBatch &operator=(DiscreteDivBatch &&) noexcept;
  DiscreteDivBatch(const DiscreteDivBatch &) = delete;
  DiscreteDivBatch &operator=(const DiscreteDivBatch &) = delete;

  // SETUP (the cold clock's family build + validation stage): validates and routes
  // every group, builds its step maps (accurate, Richardson partner, Screen) once,
  // and sizes the workspace for the largest route. The only allocations a batch
  // makes happen here. Replaces any previous preparation. Returns the number of
  // groups refused (their contracts come back RefusedGroup).
  [[nodiscard]] std::size_t prepare(std::span<const DiscreteDivBatchGroup> groups);

  [[nodiscard]] std::size_t groups() const noexcept;
  [[nodiscard]] bool group_ok(std::size_t group) const noexcept;
  // The group's accurate route (a default route when !group_ok).
  [[nodiscard]] const DiscreteDivLatticeRoute &route(std::size_t group) const noexcept;

  // THE LOOPS. One result per contract, in order; no heap allocation per contract
  // on the priced path (tests/american_discrete_div_production_alloc_test.cpp). A
  // refusal builds its error text internally (one std::string, as every Result of
  // this library does) before it is folded into the outcome code.
  // InvalidArgument: `out.size() != contracts.size()`.
  [[nodiscard]] Status price(std::span<const DiscreteDivBatchContract> contracts,
                             ExerciseStyle exercise, std::span<DiscreteDivBatchResult> out);
  [[nodiscard]] Status implied_vol(std::span<const DiscreteDivBatchContract> contracts,
                                   const DiscreteDivIvOpts &iv_opts,
                                   std::span<DiscreteDivBatchResult> out);
  // The event-aware bundle (american_discrete_div_production_greeks, no per-event
  // dividend sensitivities) of each contract at its `sigma`; a contract whose group
  // was refused, or whose bundle refused, gets NaN fields and every bit unsupported.
  [[nodiscard]] Status greeks(std::span<const DiscreteDivBatchContract> contracts,
                              ExerciseStyle exercise, double roll_horizon,
                              std::span<DiscreteDivProductionGreeks> out);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace atx::vol
