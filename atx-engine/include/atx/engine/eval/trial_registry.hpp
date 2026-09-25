#pragma once

// atx::engine::eval — TrialRegistry: the single audited count of every trial a
// research process ran, the effective number of INDEPENDENT trials, and the
// trial clusters the Deflated Sharpe benchmark is built from.
//
// ===========================================================================
//  Why
// ===========================================================================
//  Every selection-bias correction (DSR, MinTRL, the Harvey-Liu haircut, FDR)
//  needs N, the number of trials the winner was selected from. Counting the
//  miner's expressions but not the combiner/stacker/regime/optimizer hyper-
//  parameter sweeps under-deflates; counting 10,000 near-duplicate expressions
//  as 10,000 independent draws over-deflates. The registry fixes both: ONE
//  append-only log records every trial of every kind, n_eff measures how many
//  of them were independent, and accounting() clusters them (ONC) for the
//  cluster-N Deflated Sharpe (trial_clusters.hpp, deflated_sharpe.hpp).
//
// ===========================================================================
//  Semantics
// ===========================================================================
//  * Content-addressed. A trial's id is a stable 64-bit digest of
//    (kind, config_hash). Recording the same (kind, config) again is a no-op
//    that returns the existing id (inserted == false), so re-running a sweep —
//    or recording every CV FOLD of one configuration (L-08) — never inflates
//    N: the registry counts CONFIGURATIONS. Re-evaluating one configuration on
//    another window is not a new selection trial either; a caller that treats
//    windows as distinct trials folds the window into config_hash.
//  * Calendar and windows (E-16). cfg.pnl_len is the CALENDAR length C: every
//    trial's pnl covers a contiguous window [window_start, window_end] of
//    calendar periods (inclusive, end < C, at least 3 periods), so trials over
//    different windows (train / validation / walk-forward folds) share one
//    registry. The legacy record() overload is the window [0, C-1] (exactly C
//    values), which reproduces the pre-E-16 registry bit-for-bit.
//  * Per-trial metadata (E-16): window, fidelity level, family / theme tags,
//    and the in-sample / out-of-sample flag. The flag is the CALLER's claim
//    about the pnl it records (equity-mine records TRAIN pnl, i.e. InSample);
//    legacy records carry TrialSample::Unspecified.
//  * n_raw  : number of distinct trials.
//  * var_sr / mean_sr / max_sr : Welford moments of the per-trial Sharpe the
//    caller supplies (per-period, non-annualized — the DSR convention).
//  * n_eff  : participation ratio of the trials' pnl correlation matrix,
//        N_eff = (tr C)² / ||C||_F² = N² / Σ_ij ρ_ij²          (breadth.hpp),
//    with the finite-sample bias of ρ̂² removed: under independence
//    E[ρ̂²] = c > 0, which alone caps the uncorrected ratio at ~T. Using
//    ρ² ≈ (ρ̂² − c)/(1 − c) per pair, summed over off-diagonal pairs, gives
//    N_eff ≈ N for independent trials and exactly 1 for identical ones.
//    Clamped to [1, n_raw] (0 when empty). n_eff_uncorrected keeps the raw
//    participation ratio. When every window is the full calendar c = 1/(C−1)
//    (the legacy value); with partial windows c is the pair-average of
//    L_ij / sqrt(n_i(n_i−1) n_j(n_j−1)) (L_ij = overlap), kept streaming.
//    n_eff is a DIAGNOSTIC: pairing it with the cross-trial var_sr double-
//    discounts correlation (E-01); the DSR uses clusters (deflated_sharpe.hpp).
//  * The ratio is maintained STREAMING: every trial's pnl is standardized over
//    its own window to a unit vector z placed on the calendar, projected to
//    s = S·z, and folded into the d × d Gram M += s·sᵀ. Then
//    Σ_ij (s_i·s_j)² = ||M||_F². When sketch_dim >= C, S is the identity
//    (exact). Otherwise S is a count-sketch (one ±1 per calendar period,
//    seeded, O(T) to apply), the sketch is renormalized to unit length (so
//    s_i·s_j estimates the cosine, i.e. ρ over the overlap, with no
//    multiplicative norm error — identical trials give exactly 1), and the
//    pairwise sketch noise (variance ≈ 1/d) is folded into the bias constant.
//    Cost per record: O(T + d²/2); Gram memory O(d²). With keep_sketches (the
//    default) the unit sketches are also retained (8·d bytes per trial, the
//    same as a durable record) so accounting() can cluster the trials and run
//    the Monte-Carlo max-Sharpe null; accounting() itself is O(n²·d + n²·k).
//  * Memory is O(d² + n·d) with keep_sketches and O(d² + n) without: every
//    trial also keeps its TrialInfo (trials(), 72 B) and its id in the dedup
//    set. The pre-W0 "O(d²) regardless of trial count" no longer holds by
//    default: 10^6 trials at d = 64 retain ~0.6 GB with sketches, ~0.1 GB
//    without. Registries of 10^5+ trials (benchmarks, bulk sweeps) set
//    keep_sketches = false; accounting() is capped at max_trials anyway.
//  * registry_hash chains (id, sharpe bits) of the distinct trials in append
//    order (unchanged since V1): two registries with the same history have the
//    same hash.
//
// ===========================================================================
//  Durability, tamper evidence, multiple writers
// ===========================================================================
//  open(path) creates or replays an append-only binary log. Each record is
//  fixed-size and carries its own checksum; it is written (handed to the OS)
//  before record() returns. On reopen, a torn tail (a crash mid-append: a
//  trailing partial record, or a bad-checksum record that is the LAST one in
//  the file) is detected by size or checksum and truncated, so the registry
//  comes back exactly as of the last completed record. A bad-checksum record
//  with more data after it cannot come from a crash: open() returns
//  Err(ParseError) and leaves the file untouched rather than deleting
//  acknowledged trials (which would undercount N). The header pins the
//  calendar (pnl_len), sketch_dim and sketch_seed; reopening with a different
//  config is an Err. All digests are this file's own stable FNV-1a/splitmix64
//  functions, never std::hash, so ids and the file format are identical
//  across processes.
//
//  Log formats (TrialLogFormat): V1 (magic ATXTRG01, 40 + 8·d byte records:
//  id, config_hash, sharpe, kind, sketch, checksum) is still read and appended
//  to — an existing V1 log keeps its format, and only accepts legacy-shaped
//  records (full window, no metadata). V2 (magic ATXTRG02, the default for new
//  logs) adds the metadata: 72 + 8·d byte records (id, config_hash, sharpe,
//  kind|fidelity|sample, window_start, window_end, family_tag, theme_tag,
//  sketch, checksum).
//
//  Tamper evidence: chain_head() = {records, head}, where head chains a stable
//  digest of every log record's full bytes in log order. It is meant to be
//  EXPORTED OUTSIDE the log (a run manifest, write_chain_head() to a sidecar
//  kept elsewhere). open(path, cfg, anchor) verifies the log against such an
//  anchor BEFORE repairing anything: fewer records than the anchor (records
//  removed — undetectable from inside the log) or a different chain value
//  after anchor.records records (a record edited and re-checksummed) is
//  Err(ParseError), with the file left untouched. Records appended after the
//  anchor are accepted.
//
//  Multiple writers: every durable operation (open's replay, record(),
//  refresh()) runs under an exclusive OS file lock on the log (LockFileEx /
//  flock; released automatically if the process dies). record() first ingests
//  every record other handles / processes appended since this handle last
//  looked, re-checks the content address against that fresh state, then
//  appends. So two handles on one path never register the same configuration
//  twice and never interleave partial records; a log that SHRANK under a
//  handle is Err(ParseError). summary() / accounting() reflect this handle's
//  state as of its last record() / refresh().
//
//  Thread-safety: one TrialRegistry object is single-threaded; use one handle
//  per thread (or a mutex) — handles on the same path coordinate via the lock.

#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"                 // atx::core::Result
#include "atx/core/types.hpp"                 // atx::f64, atx::u8, atx::u64, atx::usize
#include "atx/engine/eval/trial_clusters.hpp" // OncConfig, TrialClusters, McMaxNull

namespace atx::engine::eval {

enum class TrialKind : atx::u8 {
  MinerExpr = 0,
  CombinerHyper = 1,
  StackHyper = 2,
  RegimeCount = 3,
  OptimizerHyper = 4,
};

// Durable log format. New logs default to V2; V1 stays readable/appendable.
enum class TrialLogFormat : atx::u8 {
  V1 = 1, // pre-E-16: fixed full-calendar pnl, no metadata
  V2 = 2, // E-16: windows, fidelity, family/theme tags, IS/OOS flag
};

// Which sample the recorded pnl comes from (E-16).
enum class TrialSample : atx::u8 {
  Unspecified = 0, // legacy records (the V1 doc said "OOS", callers recorded train)
  InSample = 1,    // train / fitting-window pnl
  OutOfSample = 2, // validation / holdout pnl
};

struct TrialId {
  atx::u64 value{};
  friend bool operator==(TrialId, TrialId) = default;
};

// Stable content address of a trial (same across processes and platforms).
[[nodiscard]] TrialId trial_id(TrialKind kind, atx::u64 config_hash) noexcept;

// Stable 64-bit tag of a family / theme name (never 0 for a non-empty name;
// 0 for the empty name, meaning "untagged").
[[nodiscard]] atx::u64 trial_tag(std::string_view name) noexcept;

// Per-trial metadata (E-16). The default is the legacy shape except for the
// window, which the caller must set (see TrialRegistry::record).
struct TrialMeta {
  atx::u64 window_start{}; // first calendar period of the pnl (inclusive)
  atx::u64 window_end{};   // last calendar period (inclusive); pnl.size() == end - start + 1
  atx::u8 fidelity{};      // evaluation fidelity level (caller-defined; 0 = full fidelity)
  TrialSample sample{TrialSample::Unspecified};
  atx::u64 family_tag{}; // trial_tag(family) or 0
  atx::u64 theme_tag{};  // trial_tag(theme) or 0
  friend bool operator==(const TrialMeta &, const TrialMeta &) = default;
};

// One registered trial, in registration order.
struct TrialInfo {
  TrialId id{};
  TrialKind kind{TrialKind::MinerExpr};
  atx::u64 config_hash{};
  atx::f64 sharpe{};
  TrialMeta meta{};
};

struct TrialRegistryConfig {
  // C: the calendar length. Every trial window lies in [0, C) and holds >= 3
  // periods; the legacy record() takes exactly C values (window [0, C-1]).
  atx::usize pnl_len{};
  // d: exact when >= pnl_len; count-sketch otherwise (>= 8). The Gram costs
  // 8·min(d, C)² bytes and each durable record 72 + 8·min(d, C) bytes (V2),
  // so keep d modest (64-256) for multi-year daily pnl.
  atx::usize sketch_dim{256};
  atx::u64 sketch_seed{0x7a1c}; // count-sketch hash key
  // Format of a NEW durable log (an existing log keeps its own; in-memory
  // registries use it for the chain-head record encoding).
  TrialLogFormat format{TrialLogFormat::V2};
  // Retain the per-trial unit sketches (needed by accounting / correlation /
  // mc_max_null): 8·d bytes per trial on top of the per-trial TrialInfo.
  // Set false for 10^5+-trial registries: memory is then O(d² + n) (header
  // note); summary(), trials() and the chain head are unchanged.
  bool keep_sketches{true};
};

struct TrialSummary {
  atx::u64 n_raw{};
  atx::f64 n_eff{};             // bias-corrected participation ratio, in [1, n_raw]
  atx::f64 n_eff_uncorrected{}; // raw participation ratio of the (sketched) Gram
  atx::f64 mean_sr{};
  atx::f64 var_sr{}; // sample variance of per-trial Sharpe (0 when n_raw < 2)
  atx::f64 max_sr{};
  atx::usize pnl_len{}; // the calendar length C
  atx::u64 registry_hash{};
  atx::u64 n_in_sample{};     // trials recorded with TrialSample::InSample
  atx::u64 n_out_of_sample{}; // trials recorded with TrialSample::OutOfSample
  atx::u64 n_unspecified{};   // trials recorded with TrialSample::Unspecified
};

struct RecordOutcome {
  TrialId id{};
  bool inserted{}; // false: (kind, config_hash) was already registered
};

// The exported, tamper-evident head of the log's record chain.
struct TrialChainHead {
  atx::u64 records{}; // number of records in the log the head covers
  atx::u64 head{};    // chained digest of those records' bytes
  friend bool operator==(const TrialChainHead &, const TrialChainHead &) = default;
};

// Sidecar codec for a chain head (one checksummed text line, written to a
// temporary file then renamed over `path`). Keep the sidecar somewhere the
// log's writers cannot rewrite if deletion of trailing records matters.
[[nodiscard]] atx::core::Status write_chain_head(const std::filesystem::path &path,
                                                 const TrialChainHead &head);
[[nodiscard]] atx::core::Result<TrialChainHead> read_chain_head(const std::filesystem::path &path);

struct TrialAccountingConfig {
  // ONC search. onc.max_k == 0 caps the base stage at min(n - 1, 64) clusters
  // (kOncDefaultMaxK); set it to at least the expected family count when a
  // registry may hold more than 64 genuine families (see AccountingDsrRule).
  OncConfig onc{};
  atx::usize mc_draws{2000};    // Monte-Carlo draws of the max-Sharpe null (>= 1)
  atx::u64 mc_seed{0x6d63ULL};  // Monte-Carlo seed
  atx::usize max_trials{4096};  // guard: the n x n correlation costs 8·n² bytes
};

// Cluster-level trial accounting (the E-01 correction's inputs).
struct TrialAccounting {
  atx::u64 n_raw{};
  TrialClusters clusters;               // ONC partition of the trials
  std::vector<atx::f64> cluster_sharpes; // representative Sharpe per cluster
  atx::f64 var_sr_clusters{};           // sample variance of cluster_sharpes (0 when < 2)
  atx::f64 var_sr{};                    // cross-trial sample variance of the Sharpes
  atx::f64 n_eff{};                     // participation ratio (diagnostic only)
  McMaxNull mc;                         // max-Sharpe null under the estimated correlation
};

class TrialRegistry {
public:
  // Durable registry backed by an append-only log at `path` (created if absent,
  // replayed and tail-repaired if present). Err(InvalidArgument) on a bad config
  // or a header/config mismatch; Err(ParseError) on a corrupt header or mid-log
  // corruption; Err(IoError) when the file cannot be used.
  [[nodiscard]] static atx::core::Result<TrialRegistry> open(const std::filesystem::path &path,
                                                             const TrialRegistryConfig &cfg);
  // As above, and verify the log against a chain head exported earlier (see
  // header note); Err(ParseError) — file untouched — when it does not match.
  [[nodiscard]] static atx::core::Result<TrialRegistry> open(const std::filesystem::path &path,
                                                             const TrialRegistryConfig &cfg,
                                                             const TrialChainHead &anchor);
  // Volatile registry (benchmarks, tests, one-shot sweeps).
  [[nodiscard]] static atx::core::Result<TrialRegistry> in_memory(const TrialRegistryConfig &cfg);

  TrialRegistry(TrialRegistry &&) noexcept;
  TrialRegistry &operator=(TrialRegistry &&) noexcept;
  TrialRegistry(const TrialRegistry &) = delete;
  TrialRegistry &operator=(const TrialRegistry &) = delete;
  ~TrialRegistry();

  // Legacy form: the window [0, pnl_len - 1] with Unspecified sample, fidelity
  // 0 and no tags. `pnl` must have exactly pnl_len finite values with non-zero
  // variance and `sharpe` must be finite, else Err(InvalidArgument) and
  // nothing is recorded. Err(IoError) if the durable append fails (the
  // in-memory state is then left unchanged).
  [[nodiscard]] atx::core::Result<RecordOutcome> record(TrialKind kind, atx::u64 config_hash,
                                                        std::span<const atx::f64> pnl,
                                                        atx::f64 sharpe);
  // E-16 form: `pnl` covers meta.window_start..meta.window_end (inclusive,
  // end < pnl_len, >= 3 periods, pnl.size() == the window length). A V1 log
  // accepts only the legacy shape (full window, fidelity 0, Unspecified, no
  // tags); anything else is Err(InvalidArgument). Same error contract as above.
  [[nodiscard]] atx::core::Result<RecordOutcome> record(TrialKind kind, atx::u64 config_hash,
                                                        const TrialMeta &meta,
                                                        std::span<const atx::f64> pnl,
                                                        atx::f64 sharpe);
  // Ingest every record other handles appended to the durable log since this
  // handle last looked; returns how many records were read (0 in memory).
  [[nodiscard]] atx::core::Result<atx::u64> refresh();

  [[nodiscard]] TrialSummary summary() const;
  [[nodiscard]] atx::u64 size() const noexcept;
  [[nodiscard]] bool contains(TrialId id) const;
  [[nodiscard]] const TrialRegistryConfig &config() const noexcept;
  // The log format in use (an existing log's own format; cfg.format otherwise).
  [[nodiscard]] TrialLogFormat format() const noexcept;
  // Every distinct trial with its metadata, in registration order.
  [[nodiscard]] const std::vector<TrialInfo> &trials() const noexcept;
  // The tamper-evident head of the record chain (export it outside the log).
  [[nodiscard]] TrialChainHead chain_head() const noexcept;

  // Row-major n x n estimated correlation (unit-sketch Gram) of the trials, in
  // trials() order. Err(InvalidArgument) without keep_sketches.
  [[nodiscard]] atx::core::Result<std::vector<atx::f64>> correlation() const;
  // Monte-Carlo null of max_i SR_i under the estimated correlation, with trial
  // i's null Sharpe sd = 1/sqrt(window length). Err(InvalidArgument) without
  // keep_sketches, when empty, or draws == 0.
  [[nodiscard]] atx::core::Result<McMaxNull> mc_max_null(atx::usize draws, atx::u64 seed) const;
  // ONC clusters + representative Sharpes + the Monte-Carlo null (E-01).
  // Err(InvalidArgument) without keep_sketches, when empty, or n_raw >
  // cfg.max_trials.
  [[nodiscard]] atx::core::Result<TrialAccounting>
  accounting(const TrialAccountingConfig &cfg) const;

private:
  struct Impl;
  explicit TrialRegistry(std::unique_ptr<Impl> impl) noexcept;
  [[nodiscard]] static atx::core::Result<TrialRegistry>
  open_impl(const std::filesystem::path &path, const TrialRegistryConfig &cfg,
            const TrialChainHead *anchor);
  std::unique_ptr<Impl> impl_;
};

} // namespace atx::engine::eval
