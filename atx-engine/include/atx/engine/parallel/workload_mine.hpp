#pragma once

// atx::engine::parallel — serialized Factory::mine_into per-genome scoring map over
// the PROCESS boundary (S7.5d — the SPRINT CAPSTONE).
//
// THE DETERMINISM CONTRACT (read before touching this file):
//   library::admit is a STATEFUL FOLD (library-wide F6 dedup, pool-wide MAX-|corr|,
//   AlphaId assigned in ADMISSION ORDER, segment_crc/integrity_crc fold base_alpha_id).
//   The admit fold therefore CANNOT be partitioned/merged byte-identically — the
//   frozen plan §0.4 "N partition libraries -> merge manifests" is UNSOUND and is NOT
//   implemented (see §0.9 amendment). The SOUND design implemented here:
//
//   * Parallelize ONLY the PURE expensive per-genome MAP: compile + evaluate +
//     extract_streams (-> the realized alpha::AlphaStreams) PLUS the pool-aware
//     fitness (dsr, raw) scored against the RUN-START pool SNAPSHOT (a const at run
//     start). This map has NO cross-item shared mutable state — exactly like the
//     eval/backtests/cpcv maps S7.5a-c lifted. dsr is POOL-INDEPENDENT (it is the
//     deflated Sharpe of the candidate's OWN OOS stream at the fixed trial count),
//     so the worker reproduces it bit-identically; raw's only pool-dependent term is
//     the MAX-|corr| redundancy against the run-start snapshot, which the worker
//     rebuilds from the serialized admitted-pnl snapshot (same SimHash seed/T/K as
//     library::worst_corr_to_pool -> the SAME value).
//   * The PARENT then runs the EXISTING deterministic rank (rank_by_deflated_fitness:
//     DESC dsr, then raw, then canon_hash, then idx) and the EXISTING SEQUENTIAL
//     library::admit loop, fed the gathered per-genome streams. Because rank+admit is
//     the identical single-process code path in the parent, report.digest (and the
//     library version_id) is byte-identical BY CONSTRUCTION across every substrate
//     and worker count.
//
// THE SEAM (mirrors workload_eval / workload_streams):
//   * INPUT: {genomes (flat Expr arenas + op-name dictionary + string pool),
//     run-start pool snapshot (admitted pnl streams + SimHash seed/T), panel (REUSED
//     S7.5c serialize_eval_input), config scalars (FitnessCfg + WeightPolicy +
//     commission)} serialized into ONE read-only InputView. MineInputView::parse
//     validates EVERY region [off,off+len) before any span/memcpy; all untrusted-
//     input failures -> Err, never ATX_ASSERT.
//   * BODY: mine_shard (a registered free-function ShardFn under WorkloadId::Mine).
//     It parses the InputView, rebuilds genome k (op-name remap -> re-analyze), runs
//     the SAME detail_eval_streams-equivalent + pool-aware fitness the in-process
//     path runs, and writes a FIXED-SHAPE output slot.
//   * OUTPUT slot per genome k: { ok:u32 (1 iff compiled+scored), pad:u32, dsr:f64,
//     raw:f64, pnl[n_periods]:f64, pos[n_periods*n_instruments]:f64 }. A genome that
//     fails compile/eval -> ok=0 (the parent treats it EXACTLY as the in-process path
//     treats a fitness/eval error: dsr=raw=0, sorts last, no streams admitted).
//
// ===========================================================================
//  WIRE LAYOUT — fixed little-endian, 8-byte f64/u64 alignment (load-bearing)
// ===========================================================================
//  ShmSegment maps `u64 length header + payload` at a page-aligned base, so the
//  PAYLOAD (InputView byte 0) is 8-aligned. Every f64/u64 array sits at an 8-aligned
//  payload offset; f64 bit patterns are copied VERBATIM (no float reformatting) so
//  the SAME inputs serialize to the SAME bytes — bit-exact determinism.
//
//  MINE payload:
//    [0  .. 4 )  u32   magic = kMineMagic
//    [4  .. 8 )  u32   n_genomes
//    [8  .. 12)  u32   n_periods          (T; the pool-snapshot stream length)
//    [12 .. 16)  u32   pool_n_alphas      (admitted alphas in the run-start snapshot)
//    [16 .. 20)  u32   trial_count        (FitnessCfg.trial_count, the deflation N)
//    [20 .. 24)  u32   cpcv_n_groups      (FitnessCfg.cpcv geometry)
//    [24 .. 28)  u32   cpcv_n_test_groups
//    [28 .. 32)  u32   wp_transform       (0=Rank 1=ZScore 2=Raw)
//    [32 .. 36)  u32   wp_flags           (bit0 industry_neutral, bit1 dollar_neutral)
//    [36 .. 40)  u32   commission_mode    (0=PerShare 1=PerDollar)
//    [40 .. 44)  u32   pad0
//    [44 .. 48)  u32   pad1
//    [48 .. 56)  f64   pool_seed          (the SimHash master seed bits; 8-aligned)
//    [56 .. 64)  f64   fit_book_size
//    [64 .. 72)  f64   cpcv_embargo       (FitnessCfg.cpcv.embargo — an f64 FRACTION)
//    [72 .. 80)  f64   wp_gross_leverage
//    [80 .. 88)  f64   wp_truncation
//    [88 .. 96)  f64   wp_winsorize_limit
//    [96 ..104)  f64   commission_per_dollar_bps
//    POOL SNAPSHOT pnl: pool_n_alphas * T f64, alpha-major (admitted-alpha pnl in
//      AlphaId order; the worker rebuilds the SimHash corr index from these). 8-aligned.
//    PANEL BLOB: a length-prefixed (u64) embedded serialize_eval_input(progs={}, panel)
//      buffer — the S7.5c eval payload with ZERO programs (panel-only). Preceded by an
//      8-pad so the embedded buffer (itself 8-aligned-internally) starts 8-aligned. The
//      worker parses it via EvalInputView and rebuilds the borrowed Panel.
//    PAD to 8.
//    GENOME OFFSET TABLE: (n_genomes + 1) u64 byte-offsets FROM PAYLOAD START.
//    GENOME REGION: each genome serialized field-for-field (see below).
//
//  Per-genome serialization (round-trips through re-analyze to the SAME streams):
//    u64   canon_hash                      (carried verbatim; 0 on the S3 search path)
//    u32   n_strings   then per string { u32 len; len bytes }   (the Ast string pool)
//    u32   n_nodes     then per Expr-node a FIXED wire record (see MineExprWire):
//            u8  kind; u8 dollar; u8 opcode; u8 n_hparams;
//            u32 name_id; u32 a; u32 b; u32 c;
//            u32 op_name_idx (Call only; 0xFFFFFFFF if not a Call);
//            f64 value; f64 hparams[2];
//    u32   root        (the single root ExprId)
//    u32   n_op_names  then per name { u32 len; len bytes }      (Call op-name dictionary)
//  (op_name_idx indexes the per-genome op-name dictionary; the worker remaps each
//   name via lib.find(name) -> Err if absent (untrusted). Re-analyze rebuilds the
//   Analysis purely; the round-trip test proves the rebuilt genome compiles+evals to
//   the SAME streams + canon_hash as the original.)
//
//  Header-only declarations; the serializer + parse view + shard live in the .cpp
//  (they pull in the heavy factory / alpha / library headers, kept out of this header).

#include <cstddef> // std::byte
#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp" // Result, Status
#include "atx/core/types.hpp" // f64, u8, u32, u64, usize

#include "atx/engine/alpha/streams.hpp"      // alpha::AlphaStreams (per-genome output gather)
#include "atx/engine/exec/execution_sim.hpp" // exec::ExecutionSimulator (config rebuild)
#include "atx/engine/factory/fitness.hpp"    // factory::FitnessCfg, FitnessReport
#include "atx/engine/factory/genome.hpp"     // factory::Genome
#include "atx/engine/loop/weight_policy.hpp" // engine::WeightPolicy (config rebuild)
#include "atx/engine/parallel/executor.hpp"  // InputView, ShardId

namespace atx::engine::alpha {
class Library; // run-wide op catalogue (genome op-name remap); fwd-declared
class Panel;
} // namespace atx::engine::alpha

namespace atx::engine::parallel {

// Magic tag ('ATXM' big-endian) the parse view checks before trusting a buffer — a
// wrong-workload or corrupt InputView is rejected, never misread.
inline constexpr atx::u32 kMineMagic = 0x4154584DU; // 'A''T''X''M'

// Fixed header byte size (a multiple of 8 so the trailing f64 arrays are 8-aligned
// within the 8-aligned payload). Exposed for the serializer + tests.
inline constexpr atx::usize kMineHeaderBytes = 104; // 12*u32 + 7*f64

// Per-genome output slot scalar header byte size: { ok:u32, pad:u32, dsr:f64, raw:f64 }.
inline constexpr atx::usize kMineSlotHeaderBytes = 24; // 2*u32 + 2*f64

// ===========================================================================
//  MineWorkItem — the parent's per-genome input bundle (built once, then serialized).
//
//  The parent owns the run-wide borrows (panel/policy/sim) and the run-start pool
//  snapshot; serialize_mine_input folds them with the genomes into one InputView.
// ===========================================================================
struct MineWorkItem {
  // The run-start admitted-pnl snapshot (alpha-major [pool_n_alphas * n_periods]).
  // EMPTY when the run-start pool is empty (worst_corr == 0 for every candidate).
  std::span<const atx::f64> pool_pnl_flat;
  atx::usize pool_n_alphas{0};
  atx::usize n_periods{0}; // T: the snapshot stream length (0 when pool is empty)
  atx::u64 pool_seed{0};   // the SimHash master seed (library master_seeds.front())
};

// ===========================================================================
//  serialize_mine_input — (genomes + pool snapshot + panel + config) -> LE buffer.
//
//  Lays out the MINE payload documented above. The genomes' Ast arenas are serialized
//  field-for-field with each Call's OpSig* replaced by its op-NAME (remapped in the
//  worker via lib.find). The panel is embedded as a panel-only serialize_eval_input
//  blob (REUSE, not copy-paste). Pure; deterministic; the same inputs always yield the
//  same bytes. ATX_ASSERTs (programmer-side, trusted input) that dimensions fit u32.
// ===========================================================================
[[nodiscard]] std::vector<std::byte> serialize_mine_input(
    std::span<const atx::engine::factory::Genome> genomes, const MineWorkItem &pool,
    const atx::engine::alpha::Panel &panel, const atx::engine::factory::FitnessCfg &fit_cfg,
    const atx::engine::WeightPolicy &policy, const atx::engine::exec::ExecutionSimulator &sim);

// ===========================================================================
//  MineInputView — parse view over a serialized mine buffer.
//
//  parse() validates the magic and EVERY region [off, off+len) against the buffer
//  size BEFORE forming any span / doing any memcpy (rejecting a truncated / oversized
//  / mis-tagged buffer with Err — no OOB, mirroring workload_eval discipline). The
//  pool-snapshot pnl span ALIASES the InputView bytes (zero copy); the embedded panel
//  blob slice is validated; the genome offset table is validated monotonic + in-bounds.
//  Genomes are deserialized on demand via genome(k) against the worker's Library.
//
//  LIFETIME: pool_pnl()'s span + the panel blob alias the InputView; valid only while
//  the backing bytes outlive this view (the SHM segment / heap buffer). Non-owning over
//  the f64 pnl + panel blob; owning over the offset table copy.
// ===========================================================================
class MineInputView {
public:
  // Parse + validate `in`. Err(InvalidArgument) on a bad magic, a header/region that
  // does not fit, a non-monotonic / out-of-bounds offset table, a misaligned f64
  // region, or a dimension/genome-count overflow.
  [[nodiscard]] static atx::core::Result<MineInputView> parse(InputView in);

  [[nodiscard]] atx::usize n_genomes() const noexcept { return n_genomes_; }
  [[nodiscard]] atx::usize n_periods() const noexcept { return n_periods_; }
  [[nodiscard]] atx::usize pool_n_alphas() const noexcept { return pool_n_alphas_; }
  [[nodiscard]] atx::u64 pool_seed() const noexcept { return pool_seed_; }

  // The run-start admitted-pnl snapshot [pool_n_alphas * n_periods] (alias). Empty when
  // the pool is empty.
  [[nodiscard]] std::span<const atx::f64> pool_pnl() const noexcept { return pool_pnl_; }

  // The reconstructed FitnessCfg / WeightPolicy / ExecutionSimulator the worker scores
  // with (deterministic rebuilds from the serialized scalars).
  [[nodiscard]] atx::engine::factory::FitnessCfg fitness_cfg() const noexcept;
  [[nodiscard]] atx::engine::WeightPolicy weight_policy() const noexcept;
  [[nodiscard]] atx::engine::exec::ExecutionSimulator sim() const;

  // Rebuild the borrowed Panel from the embedded eval-payload blob (its field columns
  // ALIAS the InputView f64 region). Err only if the embedded blob is malformed.
  [[nodiscard]] atx::core::Result<atx::engine::alpha::Panel> panel() const;

  // Deserialize genome k (0 <= k < n_genomes()) field-for-field, remapping each Call's
  // op-name against `lib` and re-running analyze to rebuild the Analysis. Err(OutOfRange)
  // if k is out of range; Err(InvalidArgument) on a malformed genome region, an unknown
  // op name (untrusted bytes), or a genome that fails to re-analyze.
  [[nodiscard]] atx::core::Result<atx::engine::factory::Genome>
  genome(atx::usize k, const atx::engine::alpha::Library &lib) const;

private:
  atx::usize n_genomes_{};
  atx::usize n_periods_{};
  atx::usize pool_n_alphas_{};
  atx::u64 pool_seed_{};
  atx::usize trial_count_{};
  atx::usize cpcv_n_groups_{};
  atx::usize cpcv_n_test_groups_{};
  atx::u32 wp_transform_{};
  atx::u32 wp_flags_{};
  atx::u32 commission_mode_{};
  atx::f64 cpcv_embargo_{};
  atx::f64 fit_book_size_{};
  atx::f64 wp_gross_leverage_{};
  atx::f64 wp_truncation_{};
  atx::f64 wp_winsorize_limit_{};
  atx::f64 commission_per_dollar_bps_{};
  std::span<const atx::f64> pool_pnl_;    // alias: pool_n_alphas * n_periods f64
  std::span<const std::byte> panel_blob_; // alias: embedded eval-payload (panel-only)
  std::vector<atx::u64> genome_offsets_;  // owned: (n_genomes + 1) byte-offsets
  std::span<const std::byte> payload_;    // whole InputView payload (genome slicing)
};

// ===========================================================================
//  MineFitnessSnapshot — worker-side worst_corr over the run-start pool snapshot.
//
//  Rebuilds the SAME SimHash CorrNeighborIndex library::worst_corr_to_pool uses
//  (CorrNeighborIndex(pool_seed, T, K=64), add each admitted pnl stream in AlphaId
//  order) and serves the MAX |corr| of a candidate vs. the snapshot — byte-identical
//  to LibraryPool::worst_corr at run start. An empty snapshot -> 0 for every candidate.
//  Defined in the .cpp; declared here so mine_shard + the round-trip test can build one.
// ===========================================================================

// Per-genome scored result: ok (1 iff compiled+scored), the pool-aware (dsr, raw) at the
// run-start snapshot, and the realized single-alpha streams. dsr/raw are 0 + streams
// empty when ok == 0 (the in-process "fitness error sorts last, no streams" semantics).
struct MineGenomeResult {
  atx::u32 ok{0};
  atx::f64 dsr{0.0};
  atx::f64 raw{0.0};
  atx::engine::alpha::AlphaStreams streams; // single-alpha (n_alphas == 1) on ok == 1
};

// Score one genome against a parsed MineInputView: detail_eval_streams-equivalent
// compile+eval over the reconstructed Panel, then pool-aware fitness vs. the rebuilt
// run-start snapshot. Returns ok=0 (NOT an Err) when the genome fails compile/eval/score
// — matching the in-process path's "silently dropped, sorts last" behaviour. Returns Err
// ONLY on a malformed buffer / out-of-range k (untrusted-input faults). Used by both
// mine_shard (process) and the round-trip test (in-process equivalence proof).
[[nodiscard]] atx::core::Result<MineGenomeResult>
score_one_genome(const MineInputView &v, atx::usize k, const atx::engine::alpha::Library &lib);

// ===========================================================================
//  mine_shard — the process-portable MINE body (ShardFn-compatible).
//
//  Parses `in` as a MineInputView, bounds-checks `k < n_genomes`, scores genome k via
//  score_one_genome (against the run-wide Library it resolves — see the .cpp note on the
//  worker's process-lifetime Library), and writes the FIXED-SHAPE output slot:
//    { ok:u32, pad:u32, dsr:f64, raw:f64, pnl[n_periods]:f64, pos[n_periods*n_inst]:f64 }.
//  An ALWAYS-ON guard (slot.size() >= need_bytes) precedes every memcpy (the S7.5c fix —
//  a debug-only assert an NDEBUG build elides before a memcpy is a Critical bug). A genome
//  that fails to score writes ok=0 with zeroed dsr/raw/streams (the parent reads ok=0 and
//  drops it). Returns Err only on a malformed buffer / out-of-range k / undersized slot;
//  never throws.
// ===========================================================================
[[nodiscard]] atx::core::Status mine_shard(InputView in, ShardId k,
                                           std::span<std::byte> slot) noexcept;

} // namespace atx::engine::parallel
