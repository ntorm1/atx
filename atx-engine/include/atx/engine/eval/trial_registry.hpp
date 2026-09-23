#pragma once

// atx::engine::eval — TrialRegistry: the single audited count of every trial a
// research process ran, and the effective number of INDEPENDENT trials.
//
// ===========================================================================
//  Why
// ===========================================================================
//  Every selection-bias correction (DSR, MinTRL, the Harvey-Liu haircut, FDR)
//  needs N, the number of trials the winner was selected from. Counting the
//  miner's expressions but not the combiner/stacker/regime/optimizer hyper-
//  parameter sweeps under-deflates; counting 10,000 near-duplicate expressions
//  as 10,000 independent draws over-deflates. The registry fixes both: ONE
//  append-only log records every trial of every kind, and N_eff measures how
//  many of them were actually independent.
//
// ===========================================================================
//  Semantics
// ===========================================================================
//  * Content-addressed. A trial's id is a stable 64-bit digest of
//    (kind, config_hash). Recording the same (kind, config) again is a no-op
//    that returns the existing id (inserted == false), so re-running a sweep
//    never inflates N.
//  * n_raw  : number of distinct trials.
//  * var_sr / mean_sr / max_sr : Welford moments of the per-trial Sharpe the
//    caller supplies (per-period, non-annualized — the DSR convention).
//  * n_eff  : participation ratio of the trials' OOS-pnl correlation matrix,
//        N_eff = (tr C)² / ||C||_F² = N² / Σ_ij ρ_ij²          (breadth.hpp),
//    with the finite-sample bias of ρ̂² removed: under independence
//    E[ρ̂²] = c > 0, which alone caps the uncorrected ratio at ~T. Using
//    ρ² ≈ (ρ̂² − c)/(1 − c) per pair, summed over off-diagonal pairs, gives
//    N_eff ≈ N for independent trials and exactly 1 for identical ones.
//    Clamped to [1, n_raw] (0 when empty). n_eff_uncorrected keeps the raw
//    participation ratio.
//  * The ratio is maintained STREAMING: every trial's pnl is standardized to a
//    unit vector z (so z_i·z_j = ρ_ij), projected to s = S·z, and folded into
//    the d × d Gram M += s·sᵀ. Then Σ_ij (s_i·s_j)² = ||M||_F². When
//    sketch_dim >= pnl_len, S is the identity (exact). Otherwise S is a
//    count-sketch (one ±1 per period, seeded, O(T) to apply) and the pairwise
//    sketch noise (variance ≈ 1/d) is folded into the bias constant c.
//    Cost per record: O(T + d²/2); memory O(d²) regardless of trial count.
//  * registry_hash chains (id, sharpe bits) in append order: two registries
//    with the same history have the same hash.
//
// ===========================================================================
//  Durability
// ===========================================================================
//  open(path) creates or replays an append-only binary log. Each record is
//  fixed-size and carries its own checksum; it is written and flushed before
//  record() returns. On reopen, a torn / corrupt tail (a crash mid-append) is
//  detected by size or checksum and truncated, so the registry comes back
//  exactly as of the last completed record. The header pins pnl_len,
//  sketch_dim and sketch_seed; reopening with a different config is an Err.
//  All digests are this file's own stable FNV-1a/splitmix64 functions, never
//  std::hash, so ids and the file format are identical across processes.
//
//  Thread-safety: none (single writer). Wrap in a mutex to share.

#include <filesystem>
#include <memory>
#include <span>

#include "atx/core/error.hpp" // atx::core::Result
#include "atx/core/types.hpp" // atx::f64, atx::u8, atx::u64, atx::usize

namespace atx::engine::eval {

enum class TrialKind : atx::u8 {
  MinerExpr = 0,
  CombinerHyper = 1,
  StackHyper = 2,
  RegimeCount = 3,
  OptimizerHyper = 4,
};

struct TrialId {
  atx::u64 value{};
  friend bool operator==(TrialId, TrialId) = default;
};

// Stable content address of a trial (same across processes and platforms).
[[nodiscard]] TrialId trial_id(TrialKind kind, atx::u64 config_hash) noexcept;

struct TrialRegistryConfig {
  atx::usize pnl_len{};          // T: every recorded OOS pnl has exactly this length (>= 3)
  atx::usize sketch_dim{256};    // d: exact when >= pnl_len; count-sketch otherwise (>= 8)
  atx::u64 sketch_seed{0x7a1c}; // count-sketch hash key
};

struct TrialSummary {
  atx::u64 n_raw{};
  atx::f64 n_eff{};             // bias-corrected participation ratio, in [1, n_raw]
  atx::f64 n_eff_uncorrected{}; // raw participation ratio of the (sketched) Gram
  atx::f64 mean_sr{};
  atx::f64 var_sr{}; // sample variance of per-trial Sharpe (0 when n_raw < 2)
  atx::f64 max_sr{};
  atx::usize pnl_len{};
  atx::u64 registry_hash{};
};

struct RecordOutcome {
  TrialId id{};
  bool inserted{}; // false: (kind, config_hash) was already registered
};

class TrialRegistry {
public:
  // Durable registry backed by an append-only log at `path` (created if absent,
  // replayed and tail-repaired if present). Err(InvalidArgument) on a bad config
  // or a header/config mismatch; Err(IoError) when the file cannot be used.
  [[nodiscard]] static atx::core::Result<TrialRegistry> open(const std::filesystem::path &path,
                                                             const TrialRegistryConfig &cfg);
  // Volatile registry (benchmarks, tests, one-shot sweeps).
  [[nodiscard]] static atx::core::Result<TrialRegistry> in_memory(const TrialRegistryConfig &cfg);

  TrialRegistry(TrialRegistry &&) noexcept;
  TrialRegistry &operator=(TrialRegistry &&) noexcept;
  TrialRegistry(const TrialRegistry &) = delete;
  TrialRegistry &operator=(const TrialRegistry &) = delete;
  ~TrialRegistry();

  // Register a trial. `oos_pnl` must have exactly pnl_len finite values with
  // non-zero variance and `sharpe` must be finite, else Err(InvalidArgument)
  // and nothing is recorded. Err(IoError) if the durable append fails (the
  // in-memory state is then left unchanged).
  [[nodiscard]] atx::core::Result<RecordOutcome> record(TrialKind kind, atx::u64 config_hash,
                                                        std::span<const atx::f64> oos_pnl,
                                                        atx::f64 sharpe);

  [[nodiscard]] TrialSummary summary() const;
  [[nodiscard]] atx::u64 size() const noexcept;
  [[nodiscard]] bool contains(TrialId id) const;
  [[nodiscard]] const TrialRegistryConfig &config() const noexcept;

private:
  struct Impl;
  explicit TrialRegistry(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

} // namespace atx::engine::eval
