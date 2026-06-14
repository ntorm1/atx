#pragma once

// atx::engine::parallel — serialized parallel_evaluate over the PROCESS boundary
// (S7.5c). The REAL `parallel_evaluate` map body (Engine::evaluate of one Program
// over a Panel) lifted onto the process-portable registered-WorkloadId +
// serialized-InputView seam, exactly mirroring S7.5b's workload_streams.{hpp,cpp}.
//
// THE WHOLE POINT (read before touching this file): the in-process IExecutor&
// overload of parallel_evaluate (S7.5a) cannot cross a process boundary — its map
// body is a captured closure over per-worker stateful Engines, and a worker is a
// SEPARATE address space. To run it on a ProcessExecutor we replace the closure
// with the seam's currency:
//
//   * INPUT: the bytecode Programs PLUS the Panel are serialized into ONE read-only
//     byte region (InputView), copied once into a PROT_READ shared segment, with N
//     concurrent worker readers (R5). serialize_eval_input builds it; EvalInputView
//     parses it and the f64 column spans ALIAS those bytes (zero copy of the big
//     Panel arrays) — every region length is validated against the buffer BEFORE a
//     span is formed (no OOB — the SAME discipline as workload_streams / ShmSegment).
//   * BODY: a PURE FREE FUNCTION ShardFn (eval_shard) — a plain function pointer,
//     registered under WorkloadId::Eval, callable in any process that links
//     atx::engine. It parses the InputView, rebuilds Panel k's borrowed view +
//     Program k, runs the SAME Engine::evaluate the in-process path uses, and
//     memcpy's the resulting alpha value columns into its pre-indexed output slot.
//   * OUTPUT: per shard k (== program k), `nroots_k` columns of `cells` f64 written
//     ONLY by shard k (R4). The parent already holds the root NAMES (from the
//     serialized programs), so names never cross back; the worker writes ONLY the
//     verbatim f64 values. The parent gathers slots in (program order, then root
//     order) into the returned SignalSet — byte-identical to the in-process
//     parallel_evaluate and the single-thread batch path, invariant across worker
//     counts {1,N} (R1/§0.5).
//
// SEPARATE TU (NOT folded into workload_streams.*): eval pulls in alpha/vm.hpp +
// alpha/bytecode.hpp + alpha/panel.hpp (heavy headers the streams workloads don't
// touch), so it lives in its own focused TU per the agent profile's "keep headers
// clean / minimal includes" rule.
//
// ===========================================================================
//  WIRE LAYOUT — fixed little-endian, 8-byte f64/u64 alignment (load-bearing)
// ===========================================================================
//  ShmSegment maps `u64 length header + payload` at a page-aligned base, so the
//  PAYLOAD (InputView byte 0) is 8-aligned. The fixed header is a multiple of 8 so
//  the Panel f64 columns immediately after it are 8-aligned by construction; the
//  programs offset table (u64) is preceded by an explicit pad to an 8-boundary so
//  its entries are 8-aligned too. f64 bit patterns are copied VERBATIM (no float
//  reformatting), so the SAME (programs, panel) serializes to the SAME bytes — bit-
//  exact determinism.
//
//  EVAL payload:
//    [0  .. 4 )  u32   magic = kEvalMagic
//    [4  .. 8 )  u32   n_programs
//    [8  .. 12)  u32   dates
//    [12 .. 16)  u32   instruments
//    [16 .. 20)  u32   n_fields
//    [20 .. 24)  u32   universe_present  (0/1)
//    [24 .. 32)  u64   cells             (== dates * instruments; stored)
//    [32 .. 32 + 8*F0)        f64[]  panel columns, column-major: n_fields blocks of
//                                     `cells` f64 each (field f's field_all() span).
//                                     F0 = n_fields * cells. (32 % 8 == 0 -> 8-aligned.)
//    [.. + cells)             u8[]   universe mask (cells bytes), present iff
//                                     universe_present == 1 (always 1 in practice —
//                                     the materialized Panel mask round-trips exactly).
//    field-name DICTIONARY:   n_fields length-prefixed strings (u32 len + len bytes).
//    PAD to the next multiple of 8.
//    programs OFFSET TABLE:   (n_programs + 1) u64 byte-offsets FROM BUFFER START.
//                                     offsets[0] = start of the programs region,
//                                     offsets[k+1] = one-past program k; the table
//                                     itself begins 8-aligned (the pad above).
//    programs REGION:         each Program serialized field-for-field (see below).
//
//  Per-Program serialization (round-trips field-for-field):
//    u32   num_slots
//    u16   required_lookback
//    u32   unique_nodes
//    u32   total_ast_nodes
//    u32   peak_live_slots
//    u32   cache_hits
//    u32   intern_attempts
//    u32   n_code      then  n_code * sizeof(Instr) bytes (Instr is trivially-copyable
//                            POD -> bulk memcpy; static_assert'd in the .cpp).
//    u32   n_roots     then  per root { u32 output; u32 name_len; name_len name bytes }.
//    u32   n_pfields   then  n_pfields length-prefixed strings (Program::fields).
//  (Programs are byte-packed; only the offset table is 8-aligned. Instr is memcpy'd
//   into an OWNED std::vector<Instr> on parse — memcpy has no alignment requirement,
//   so we never form a misaligned span<const Instr> over the buffer.)
//
//  Header-only declarations; serializer + parse view + shard live in the .cpp (they
//  pull in the heavy alpha/vm + bytecode + panel headers, kept out of this header).

#include <cstddef> // std::byte
#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp" // Result, Status
#include "atx/core/types.hpp" // f64, u8, u16, u32, u64, usize

#include "atx/engine/alpha/bytecode.hpp"    // alpha::Program (Instr / Root)
#include "atx/engine/alpha/panel.hpp"       // alpha::Panel
#include "atx/engine/parallel/executor.hpp" // InputView, ShardId

namespace atx::engine::parallel {

// Magic tag ('ATXE' big-endian) the parse view checks before trusting a buffer — a
// wrong-workload or corrupt InputView is rejected, never misread.
inline constexpr atx::u32 kEvalMagic = 0x41545845U; // 'A''T''X''E'

// Fixed header byte size (a multiple of 8 so the trailing f64 columns are 8-aligned
// within the 8-aligned payload). Exposed for the serializer + tests.
inline constexpr atx::usize kEvalHeaderBytes = 32; // 6*u32 + u64

// ===========================================================================
//  serialize_eval_input — (Programs + Panel) -> fixed LE byte buffer.
//
//  Lays out the EVAL payload documented above: the 32-byte header, the Panel's f64
//  columns (verbatim raw bit patterns), the materialized universe mask, the field-
//  name dictionary, an 8-aligned programs offset table, then each Program serialized
//  field-for-field. The returned buffer is the InputView payload the ProcessExecutor
//  copies into SHM. Pure; deterministic; the same (progs, panel) always yield the
//  same bytes. ATX_ASSERTs (programmer-side, trusted input) that the Panel's
//  dimensions fit u32.
// ===========================================================================
[[nodiscard]] std::vector<std::byte>
serialize_eval_input(std::span<const atx::engine::alpha::Program> progs,
                     const atx::engine::alpha::Panel &panel);

// ===========================================================================
//  EvalInputView — parse view over a serialized eval buffer.
//
//  parse() validates the magic and EVERY region [off, off+len) against the buffer
//  size BEFORE forming any span / doing any memcpy (rejecting a truncated / oversized
//  / mis-tagged buffer with Err — no OOB, mirroring workload_streams discipline).
//  The Panel column spans ALIAS the InputView bytes (no copy of the big arrays); the
//  field-name dictionary, universe mask, and offset table are validated and copied
//  out so panel() / program(k) can hand back self-contained owned data + aliasing
//  column spans. Programs are deserialized on demand via program(k).
//
//  LIFETIME: panel()'s column spans alias the InputView; the rebuilt Panel is valid
//  only while the backing bytes outlive this view (the InputView's SHM segment / heap
//  buffer). Non-owning over the f64 columns; owning over names/universe/offsets.
// ===========================================================================
class EvalInputView {
public:
  // Parse + validate `in`. Err(InvalidArgument) on a bad magic, a header/region that
  // does not fit, a non-monotonic / out-of-bounds offset table, or a misaligned f64
  // column region.
  [[nodiscard]] static atx::core::Result<EvalInputView> parse(InputView in);

  [[nodiscard]] atx::usize n_programs() const noexcept { return n_programs_; }
  [[nodiscard]] atx::usize dates() const noexcept { return dates_; }
  [[nodiscard]] atx::usize instruments() const noexcept { return instruments_; }
  [[nodiscard]] atx::usize n_fields() const noexcept { return n_fields_; }
  [[nodiscard]] atx::usize cells() const noexcept { return cells_; }

  // Rebuild the (borrowed) Panel: field columns ALIAS the InputView f64 region, the
  // field-name dictionary + universe mask are the owned copies parse() validated.
  // Err only if Panel::create_borrowed rejects (it cannot, given parse's checks —
  // surfaced as Err for completeness, never ATX_ASSERT on untrusted input).
  [[nodiscard]] atx::core::Result<atx::engine::alpha::Panel> panel() const;

  // Deserialize program k (0 <= k < n_programs()) field-for-field from the buffer.
  // Err(OutOfRange) if k is out of range; Err(InvalidArgument) on a malformed program
  // region (a string/array length overruns the program's [off, end) slice).
  [[nodiscard]] atx::core::Result<atx::engine::alpha::Program> program(atx::usize k) const;

private:
  atx::usize n_programs_{};
  atx::usize dates_{};
  atx::usize instruments_{};
  atx::usize n_fields_{};
  atx::usize cells_{};
  bool universe_present_{};
  std::span<const atx::f64> columns_;      // alias: n_fields * cells f64 (column-major)
  std::vector<std::string> field_names_;   // owned (copied out on parse)
  std::vector<std::uint8_t> universe_;     // owned: cells bytes (all-ones if absent)
  std::span<const atx::u64> prog_offsets_; // alias: (n_programs + 1) byte-offsets
  std::span<const std::byte> payload_;     // whole InputView payload (program slicing)
};

// ===========================================================================
//  eval_shard — the process-portable EVAL body (ShardFn-compatible).
//
//  Parses `in` as an EvalInputView, bounds-checks `k < n_programs`, rebuilds the
//  borrowed Panel (aliasing the input columns; valid for this call) + Program k,
//  runs the SAME Engine::evaluate the in-process path uses, and memcpy's each root's
//  verbatim f64 value column into `slot` (slot.size() >= nroots_k * cells * 8,
//  validated). On an evaluate Err it returns that Err (the submit() seam reduces to
//  the lowest-shard-id error, matching the in-process "lowest-index program error"
//  contract). Returns Err on a malformed buffer / out-of-range k / undersized slot;
//  never throws.
// ===========================================================================
[[nodiscard]] atx::core::Status eval_shard(InputView in, ShardId k,
                                           std::span<std::byte> slot) noexcept;

} // namespace atx::engine::parallel
