#pragma once

// atx::engine::parallel — serialized AlphaStreams workloads over the PROCESS
// boundary (S7.5b). The REAL `parallel_backtests` / `parallel_cpcv` map bodies
// (run_full_backtest / run_one_fold) lifted onto the process-portable
// registered-WorkloadId + serialized-InputView seam.
//
// THE WHOLE POINT (read before touching this file): the in-process IExecutor&
// overloads of these two workloads (S7.5a) cannot cross a process boundary — their
// map body is a captured closure, and a worker is a SEPARATE address space. To run
// them on a ProcessExecutor we replace the closure with the seam's currency:
//
//   * INPUT: the AlphaStreams (two trivially-serializable flat f64 arrays plus a
//     small POD header) is serialized into ONE read-only byte region (InputView),
//     copied once into a PROT_READ shared segment, with N concurrent worker readers
//     (R5). serialize_backtests_input / serialize_cpcv_input build it; the
//     BacktestsInputView / CpcvInputView parse views ALIAS those bytes (zero copy
//     of the big f64 arrays) and validate every length against the buffer before
//     forming any span (no OOB — same discipline as ShmSegment::open).
//   * BODY: a PURE FREE FUNCTION ShardFn (backtests_shard / cpcv_shard) — a plain
//     function pointer, registered under WorkloadId::Backtests / ::Cpcv, callable
//     in any process that links atx::engine. It parses the InputView, computes the
//     SAME run_full_backtest / run_one_fold the in-process path uses, and memcpy's
//     the resulting POD FoldResult into its pre-indexed output slot.
//   * OUTPUT: one FoldResult per shard, slot s written ONLY by shard s (R4). The
//     parent gathers the slots in canonical ShardId order into the returned
//     std::vector<FoldResult> — byte-identical to the ThreadExecutor path and the
//     sequential oracle, invariant across worker counts {1,N} (R1/§0.5).
//
// ===========================================================================
//  WIRE LAYOUT — fixed little-endian, 8-byte f64 alignment (load-bearing)
// ===========================================================================
//  ShmSegment maps `u64 length header + payload` at a page-aligned base, so the
//  PAYLOAD (InputView byte 0) is 8-aligned. We therefore keep every f64 array at
//  an 8-aligned PAYLOAD OFFSET: each layout's fixed header is padded to a multiple
//  of 8, and the f64 arrays then begin (and chain) on 8-aligned offsets. The f64
//  bit patterns are copied VERBATIM (no float reformatting), so the SAME
//  AlphaStreams serializes to the SAME bytes on any run — bit-exact determinism.
//
//  BACKTESTS payload:
//    [0  .. 4 )  u32   magic = kBacktestsMagic
//    [4  .. 8 )  u32   n_alphas
//    [8  .. 12)  u32   n_periods
//    [12 .. 16)  u32   n_instruments
//    [16 .. 24)  f64   book_size                 (8-aligned: 16 % 8 == 0)
//    [24 .. 24 + 8*P0)        f64[]  pnl_flat     (P0 = n_alphas*n_periods)
//    [.. + 8*P1)              f64[]  pos_flat     (P1 = P0*n_instruments)
//  (24 is a multiple of 8, so pnl_flat and pos_flat are both 8-aligned.)
//
//  CPCV payload (folds vary in size, so a fold OFFSET TABLE precedes them):
//    [0  .. 4 )  u32   magic = kCpcvMagic
//    [4  .. 8 )  u32   n_alphas
//    [8  .. 12)  u32   n_periods
//    [12 .. 16)  u32   n_instruments
//    [16 .. 20)  u32   alpha_id
//    [20 .. 24)  u32   n_folds
//    [24 .. 32)  f64   book_size                 (8-aligned)
//    [32 .. 32 + 8*P0)        f64[]  pnl_flat     (P0 = n_alphas*n_periods)
//    [.. + 8*P1)              f64[]  pos_flat     (P1 = P0*n_instruments)
//    fold OFFSET TABLE: (n_folds + 1) u64 byte-offsets (from payload start) marking
//      each fold's test_idx region; entry[f]..entry[f+1] bounds fold f's u64 indices.
//      The table itself begins on an 8-aligned offset (pos_flat ends 8-aligned).
//    fold DATA: each fold f's test_idx as a contiguous run of u64 period indices.
//  (Both the f64 arrays and the u64 table/data sit at 8-aligned offsets.)
//
//  Header-only declarations; the parse views are inline (zero-copy span math), the
//  serializers + ShardFns live in workload_streams.cpp (they pull in parallel_run /
//  cpcv heavy headers — kept out of this widely-included header).

#include <cstddef> // std::byte
#include <span>
#include <vector>

#include "atx/core/error.hpp" // Result, Status
#include "atx/core/types.hpp" // f64, u32, u64, usize

#include "atx/engine/alpha/streams.hpp"     // alpha::AlphaStreams
#include "atx/engine/eval/cpcv.hpp"         // eval::CpcvFold
#include "atx/engine/parallel/executor.hpp" // InputView, ShardId, ShardFn-compatible signature
#include "atx/engine/parallel/fwd.hpp"      // FoldResult (POD; full def in parallel_run.hpp)

namespace atx::engine::parallel {

// Magic tags ('ATXB' / 'ATXC' big-endian) the parse views check before trusting a
// buffer — a wrong-workload or corrupt InputView is rejected, never misread.
inline constexpr atx::u32 kBacktestsMagic = 0x41545842U; // 'A''T''X''B'
inline constexpr atx::u32 kCpcvMagic = 0x41545843U;      // 'A''T''X''C'

// Fixed header byte sizes (both multiples of 8 so the trailing f64 arrays are
// 8-aligned within the 8-aligned payload). Exposed for the serializer + tests.
inline constexpr atx::usize kBacktestsHeaderBytes = 24; // 4*u32 + f64
inline constexpr atx::usize kCpcvHeaderBytes = 32;      // 6*u32 + f64

// ===========================================================================
//  serialize_backtests_input — AlphaStreams (+ book_size) -> fixed LE byte buffer.
//
//  Lays out the BACKTESTS payload documented above: a 24-byte header then
//  pnl_flat and pos_flat copied VERBATIM (raw f64 bit patterns). The returned
//  buffer is the InputView payload the ProcessExecutor copies into SHM. Pure;
//  deterministic; the same streams always yield the same bytes.
// ===========================================================================
[[nodiscard]] std::vector<std::byte>
serialize_backtests_input(const atx::engine::alpha::AlphaStreams& streams, atx::f64 book_size);

// ===========================================================================
//  serialize_cpcv_input — AlphaStreams (+ alpha_id, book_size, folds) -> LE buffer.
//
//  Lays out the CPCV payload: header, the two f64 arrays, then a fold offset table
//  + each fold's test_idx as u64 indices (folds vary in size). Only test_idx is
//  serialized — run_one_fold reads only the test index set (train_idx is unused by
//  the per-fold metric). Pure; deterministic.
// ===========================================================================
[[nodiscard]] std::vector<std::byte>
serialize_cpcv_input(const atx::engine::alpha::AlphaStreams& streams, atx::usize alpha_id,
                     atx::f64 book_size,
                     std::span<const atx::engine::eval::CpcvFold> folds);

// ===========================================================================
//  BacktestsInputView — zero-copy parse view over a serialized backtests buffer.
//
//  parse() validates the magic and every region length against the buffer size
//  BEFORE exposing any span (rejecting a truncated / oversized / mis-tagged buffer
//  with Err — no OOB span, mirroring ShmSegment::open / read_manifest discipline).
//  pnl() / pos() ALIAS the InputView bytes (no copy of the big arrays); the
//  AlphaStreams-shaped accessors (pnl_of / positions_of) reconstruct the per-alpha
//  spans with the SAME index math AlphaStreams uses, so the shard body computes
//  bit-identically to the in-process path.
//
//  LIFETIME: the spans alias the InputView; they are valid only while the backing
//  bytes outlive this view (the InputView's SHM segment / heap buffer). Non-owning.
// ===========================================================================
class BacktestsInputView {
public:
  // Parse + validate `in`. Err(InvalidArgument) on a bad magic, a header that does
  // not fit, or array regions that overrun / underrun the buffer.
  [[nodiscard]] static atx::core::Result<BacktestsInputView> parse(InputView in) noexcept;

  [[nodiscard]] atx::usize n_alphas() const noexcept { return n_alphas_; }
  [[nodiscard]] atx::usize n_periods() const noexcept { return n_periods_; }
  [[nodiscard]] atx::usize n_instruments() const noexcept { return n_instruments_; }
  [[nodiscard]] atx::f64 book_size() const noexcept { return book_size_; }

  // The whole flat pnl array [n_alphas * n_periods] (alias).
  [[nodiscard]] std::span<const atx::f64> pnl() const noexcept { return pnl_; }
  // The whole flat pos array [n_alphas * n_periods * n_instruments] (alias).
  [[nodiscard]] std::span<const atx::f64> pos() const noexcept { return pos_; }

  // Per-alpha pnl stream (length n_periods) — same index math as AlphaStreams::pnl.
  // PRECONDITION: alpha < n_alphas() (debug-asserted; the shard bounds-checks first).
  [[nodiscard]] std::span<const atx::f64> pnl_of(atx::usize alpha) const noexcept;
  // Per-alpha, per-period position cross-section (length n_instruments) — same math
  // as AlphaStreams::positions. PRECONDITION: alpha < n_alphas, period < n_periods.
  [[nodiscard]] std::span<const atx::f64> positions_of(atx::usize alpha,
                                                       atx::usize period) const noexcept;

private:
  atx::usize n_alphas_{};
  atx::usize n_periods_{};
  atx::usize n_instruments_{};
  atx::f64 book_size_{};
  std::span<const atx::f64> pnl_;
  std::span<const atx::f64> pos_;
};

// ===========================================================================
//  CpcvInputView — zero-copy parse view over a serialized cpcv buffer.
//
//  Like BacktestsInputView, plus a fold offset table: fold(f) returns fold f's
//  test_idx as a span<const u64> aliasing the buffer. parse() validates the magic,
//  the header, the two f64 arrays, the (n_folds+1)-entry offset table, and that
//  every table entry is monotonic and in-bounds (a malformed table is rejected,
//  never used to form an OOB span).
// ===========================================================================
class CpcvInputView {
public:
  [[nodiscard]] static atx::core::Result<CpcvInputView> parse(InputView in) noexcept;

  [[nodiscard]] atx::usize n_alphas() const noexcept { return n_alphas_; }
  [[nodiscard]] atx::usize n_periods() const noexcept { return n_periods_; }
  [[nodiscard]] atx::usize n_instruments() const noexcept { return n_instruments_; }
  [[nodiscard]] atx::usize alpha_id() const noexcept { return alpha_id_; }
  [[nodiscard]] atx::usize n_folds() const noexcept { return n_folds_; }
  [[nodiscard]] atx::f64 book_size() const noexcept { return book_size_; }

  [[nodiscard]] std::span<const atx::f64> pnl() const noexcept { return pnl_; }
  [[nodiscard]] std::span<const atx::f64> pos() const noexcept { return pos_; }

  [[nodiscard]] std::span<const atx::f64> pnl_of(atx::usize alpha) const noexcept;
  [[nodiscard]] std::span<const atx::f64> positions_of(atx::usize alpha,
                                                       atx::usize period) const noexcept;

  // Fold f's test_idx (period indices), aliasing the buffer. PRECONDITION:
  // f < n_folds() (debug-asserted; the shard bounds-checks f first).
  [[nodiscard]] std::span<const atx::u64> fold_test_idx(atx::usize f) const noexcept;

private:
  atx::usize n_alphas_{};
  atx::usize n_periods_{};
  atx::usize n_instruments_{};
  atx::usize alpha_id_{};
  atx::usize n_folds_{};
  atx::f64 book_size_{};
  std::span<const atx::f64> pnl_;
  std::span<const atx::f64> pos_;
  std::span<const atx::u64> fold_offsets_; // (n_folds + 1) byte-offsets from payload start
  std::span<const std::byte> payload_;     // whole InputView payload (for fold slicing)
};

// ===========================================================================
//  backtests_shard — the process-portable BACKTESTS body (ShardFn-compatible).
//
//  Parses `in` as a BacktestsInputView, bounds-checks `a < n_alphas`, computes
//  FoldResult via the SAME run_full_backtest the in-process path uses (over a
//  non-owning AlphaStreams reconstructed from the aliased spans), and memcpy's the
//  POD FoldResult into `slot` (slot.size() >= sizeof(FoldResult), validated).
//  Returns Err on a malformed buffer / out-of-range a / undersized slot; never throws.
// ===========================================================================
[[nodiscard]] atx::core::Status backtests_shard(InputView in, ShardId a,
                                                std::span<std::byte> slot) noexcept;

// ===========================================================================
//  cpcv_shard — the process-portable CPCV body (ShardFn-compatible).
//
//  Parses `in` as a CpcvInputView, bounds-checks `f < n_folds`, computes FoldResult
//  via the SAME run_one_fold (over the reconstructed streams + fold f's test_idx),
//  and memcpy's the POD FoldResult into `slot`. Returns Err on a malformed buffer /
//  out-of-range f / undersized slot; never throws.
// ===========================================================================
[[nodiscard]] atx::core::Status cpcv_shard(InputView in, ShardId f,
                                           std::span<std::byte> slot) noexcept;

} // namespace atx::engine::parallel
