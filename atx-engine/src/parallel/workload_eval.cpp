#include "atx/engine/parallel/workload_eval.hpp"

#include <cstddef> // std::byte
#include <cstdint> // std::uint8_t, std::uintptr_t
#include <cstring> // std::memcpy
#include <span>
#include <string>
#include <type_traits> // std::is_trivially_copyable_v
#include <utility>     // std::move
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/macro.hpp" // ATX_ASSERT
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"    // alpha::Program, alpha::Instr
#include "atx/engine/alpha/panel.hpp"       // alpha::Panel, alpha::SignalSet
#include "atx/engine/alpha/vm.hpp"          // alpha::Engine
#include "atx/engine/parallel/executor.hpp" // InputView, ShardId

namespace atx::engine::parallel {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Result;
using atx::core::Status;

namespace alpha = atx::engine::alpha;

// Instr crosses the boundary as raw bytes (bulk memcpy of the code stream), so it
// MUST be trivially copyable for that memcpy to be defined. (Same wire-type
// discipline as FoldResult / the process_executor control structs.)
static_assert(std::is_trivially_copyable_v<alpha::Instr>,
              "alpha::Instr must be a trivially-copyable POD to memcpy across the wire");

namespace {

// ---------------------------------------------------------------------------
//  Little-endian POD / array writers — append raw bytes to a growing buffer. The
//  wire is fixed little-endian; on the SAME binary the byte image is stable, and
//  f64 bit patterns are copied VERBATIM (no float reformatting). No byte-swap: the
//  parent and worker are the same binary on the same machine (the cross-endian lift
//  is a recorded residual, not in scope here). Mirrors workload_streams.cpp.
// ---------------------------------------------------------------------------
template <class T> void put_pod(std::vector<std::byte> &buf, const T &v) {
  const auto *p = reinterpret_cast<const std::byte *>(
      &v); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
  buf.insert(buf.end(), p, p + sizeof(T));
}

// Append a flat f64 array's raw bytes (verbatim bit patterns).
void put_f64_array(std::vector<std::byte> &buf, std::span<const atx::f64> a) {
  if (a.empty()) {
    return;
  }
  const auto *p = reinterpret_cast<const std::byte *>(
      a.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
  buf.insert(buf.end(), p, p + a.size_bytes());
}

// Append a length-prefixed string: u32 length then the raw name bytes.
void put_string(std::vector<std::byte> &buf, const std::string &s) {
  ATX_ASSERT(s.size() <= static_cast<atx::usize>(0xFFFFFFFFU)); // trusted (compiled program)
  put_pod(buf, static_cast<atx::u32>(s.size()));
  const auto *p = reinterpret_cast<const std::byte *>(
      s.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
  buf.insert(buf.end(), p, p + s.size());
}

// Pad `buf` up to the next multiple of 8 with zero bytes (so the trailing u64 offset
// table lands 8-aligned within the 8-aligned payload).
void pad_to_8(std::vector<std::byte> &buf) {
  while ((buf.size() % 8) != 0) {
    buf.push_back(std::byte{0});
  }
}

// ---------------------------------------------------------------------------
//  Bounds-checked little-endian POD reader from a const byte span. Returns false if
//  the field does not wholly fit (the parse view rejects rather than form an OOB
//  read — same discipline as workload_streams / ShmSegment::open).
// ---------------------------------------------------------------------------
template <class T>
[[nodiscard]] bool get_pod(std::span<const std::byte> bytes, atx::usize off, T &out) noexcept {
  if (off > bytes.size() || bytes.size() - off < sizeof(T)) {
    return false;
  }
  std::memcpy(&out, bytes.data() + off, sizeof(T));
  return true;
}

// Overflow-checked usize multiply: out = a * b, false on wrap. Dimensions come from
// an UNTRUSTED buffer (u32/u64 each); a wrapped-small product would let a bogus
// region pass a fits-check and form an OOB alias. Reject the wrap instead.
[[nodiscard]] bool checked_mul(atx::usize a, atx::usize b, atx::usize &out) noexcept {
  if (a != 0 && b > (static_cast<atx::usize>(-1) / a)) {
    return false; // a * b would overflow usize
  }
  out = a * b;
  return true;
}

// Overflow-safe: does `count` f64 cells fit in [off, buf_size)? Computes the byte
// span without ever forming an out-of-range product that wraps usize.
[[nodiscard]] bool f64_region_fits(atx::usize buf_size, atx::usize off, atx::usize count) noexcept {
  if (off > buf_size) {
    return false;
  }
  const atx::usize avail = buf_size - off;
  return count <= (avail / sizeof(atx::f64)); // count * 8 <= avail, no overflow
}

// Form a span<const f64> aliasing `bytes[off, off + count*8)`. PRECONDITION: the
// caller has already validated f64_region_fits AND 8-alignment of the base ptr.
[[nodiscard]] std::span<const atx::f64> f64_alias(std::span<const std::byte> bytes, atx::usize off,
                                                  atx::usize count) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto *p = reinterpret_cast<const atx::f64 *>(bytes.data() + off);
  return std::span<const atx::f64>{p, count};
}

// True iff `ptr` is suitably aligned for type `T` (mirrors workload_streams' u64-
// table alignment check). Untrusted SHM bytes could in principle put a region at a
// misaligned offset; forming a span<const T> over a misaligned address is UB, so we
// reject it with Err rather than alias.
template <class T> [[nodiscard]] bool is_aligned(const void *ptr) noexcept {
  return (reinterpret_cast<std::uintptr_t>(ptr) % alignof(T)) ==
         0; // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

// ---------------------------------------------------------------------------
//  Bounded reader over a const byte slice — a forward cursor that NEVER reads past
//  `end`. Each read validates the field fits the REMAINING slice; on a short read it
//  sets `ok=false` and the caller bails with Err (no OOB on untrusted program bytes).
// ---------------------------------------------------------------------------
class SliceReader {
public:
  SliceReader(std::span<const std::byte> bytes, atx::usize begin, atx::usize end) noexcept
      : bytes_{bytes}, pos_{begin}, end_{end} {
    // PRECONDITION (caller-validated): begin <= end <= bytes.size().
    ATX_ASSERT(begin <= end && end <= bytes.size());
  }

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] atx::usize pos() const noexcept { return pos_; }
  [[nodiscard]] atx::usize remaining() const noexcept { return ok_ ? end_ - pos_ : 0; }

  template <class T> [[nodiscard]] bool read_pod(T &out) noexcept {
    if (!ok_ || (end_ - pos_) < sizeof(T)) {
      ok_ = false;
      return false;
    }
    std::memcpy(&out, bytes_.data() + pos_, sizeof(T));
    pos_ += sizeof(T);
    return true;
  }

  // Read a length-prefixed string (u32 len + len bytes) into `out`.
  [[nodiscard]] bool read_string(std::string &out) noexcept {
    atx::u32 len = 0;
    if (!read_pod(len)) {
      return false;
    }
    if (!ok_ || (end_ - pos_) < len) {
      ok_ = false;
      return false;
    }
    out.assign(reinterpret_cast<const char *>(bytes_.data() + pos_),
               len); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    pos_ += len;
    return true;
  }

  // Copy `count` raw bytes into `dst` (dst sized by the caller). Used for the Instr
  // code stream: memcpy into an OWNED vector<Instr> (no alignment requirement).
  [[nodiscard]] bool read_bytes(void *dst, atx::usize count) noexcept {
    if (!ok_ || (end_ - pos_) < count) {
      ok_ = false;
      return false;
    }
    if (count != 0) {
      std::memcpy(dst, bytes_.data() + pos_, count);
    }
    pos_ += count;
    return true;
  }

private:
  std::span<const std::byte> bytes_;
  atx::usize pos_{};
  atx::usize end_{};
  bool ok_{true};
};

// Serialize one Program field-for-field into `buf` (byte-packed; see the wire layout
// in the header). The 6 telemetry u32s are serialized for a faithful round-trip even
// though they do not affect evaluate. Instr is a trivially-copyable POD, so the code
// stream is one bulk memcpy of n_code * sizeof(Instr) bytes.
void serialize_program(std::vector<std::byte> &buf, const alpha::Program &prog) {
  put_pod(buf, prog.num_slots);
  put_pod(buf, prog.required_lookback);
  put_pod(buf, prog.unique_nodes);
  put_pod(buf, prog.total_ast_nodes);
  put_pod(buf, prog.peak_live_slots);
  put_pod(buf, prog.cache_hits);
  put_pod(buf, prog.intern_attempts);

  ATX_ASSERT(prog.code.size() <= static_cast<atx::usize>(0xFFFFFFFFU));
  put_pod(buf, static_cast<atx::u32>(prog.code.size()));
  if (!prog.code.empty()) {
    const auto *p = reinterpret_cast<const std::byte *>(
        prog.code.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    buf.insert(buf.end(), p, p + prog.code.size() * sizeof(alpha::Instr));
  }

  ATX_ASSERT(prog.roots.size() <= static_cast<atx::usize>(0xFFFFFFFFU));
  put_pod(buf, static_cast<atx::u32>(prog.roots.size()));
  for (const alpha::Program::Root &root : prog.roots) {
    put_pod(buf, root.output);
    put_string(buf, root.name);
  }

  ATX_ASSERT(prog.fields.size() <= static_cast<atx::usize>(0xFFFFFFFFU));
  put_pod(buf, static_cast<atx::u32>(prog.fields.size()));
  for (const std::string &f : prog.fields) {
    put_string(buf, f);
  }
}

} // namespace

// ===========================================================================
//  serialize_eval_input
// ===========================================================================
std::vector<std::byte> serialize_eval_input(std::span<const alpha::Program> progs,
                                            const alpha::Panel &panel) {
  const atx::usize n_progs = progs.size();
  const atx::usize dates = panel.dates();
  const atx::usize instruments = panel.instruments();
  const atx::usize n_fields = panel.num_fields();
  const atx::usize cells = panel.cells(); // dates * instruments

  // Trusted (programmer-side) input: the Panel's dimensions and program count must
  // fit u32 to round-trip through the fixed-width header (a fail-loud precondition,
  // NOT an untrusted-input Err).
  ATX_ASSERT(n_progs <= static_cast<atx::usize>(0xFFFFFFFFU));
  ATX_ASSERT(dates <= static_cast<atx::usize>(0xFFFFFFFFU));
  ATX_ASSERT(instruments <= static_cast<atx::usize>(0xFFFFFFFFU));
  ATX_ASSERT(n_fields <= static_cast<atx::usize>(0xFFFFFFFFU));

  std::vector<std::byte> buf;

  // Header (32 bytes, multiple of 8 -> the f64 columns land 8-aligned). We always
  // serialize the MATERIALIZED universe mask (Panel materializes empty -> all-ones),
  // so universe_present is always 1 and the worker's panel gets a bit-identical mask
  // -> bit-identical LoadField NaN-ing -> bit-identical evaluate.
  put_pod(buf, kEvalMagic);
  put_pod(buf, static_cast<atx::u32>(n_progs));
  put_pod(buf, static_cast<atx::u32>(dates));
  put_pod(buf, static_cast<atx::u32>(instruments));
  put_pod(buf, static_cast<atx::u32>(n_fields));
  put_pod(buf, static_cast<atx::u32>(1)); // universe_present
  put_pod(buf, static_cast<atx::u64>(cells));
  ATX_ASSERT(buf.size() == kEvalHeaderBytes);

  // Panel f64 columns, column-major (field f's whole field_all() span), verbatim.
  for (atx::usize f = 0; f < n_fields; ++f) {
    put_f64_array(buf, panel.field_all(static_cast<alpha::FieldId>(f)));
  }

  // Universe mask: cells bytes, reconstructed from in_universe(date, inst). This
  // materializes the SAME mask the source Panel holds (Panel stores an all-ones mask
  // when constructed with an empty universe), so the round-tripped Panel evaluates
  // identically.
  for (atx::usize d = 0; d < dates; ++d) {
    for (atx::usize i = 0; i < instruments; ++i) {
      buf.push_back(panel.in_universe(d, i) ? std::byte{1} : std::byte{0});
    }
  }

  // Field-name dictionary: n_fields length-prefixed strings. (We need the names to
  // rebuild a Panel whose field_id() resolves the program's field references.)
  for (atx::usize f = 0; f < n_fields; ++f) {
    put_string(buf, std::string{panel.field_name(static_cast<alpha::FieldId>(f))});
  }

  // Pad to 8 so the offset table (u64) is 8-aligned.
  pad_to_8(buf);

  // Programs offset table: (n_progs + 1) u64 byte-offsets FROM BUFFER START. We do
  // not know the per-program sizes until we serialize them, so reserve the table
  // slots, serialize the programs region into a side buffer, then backfill.
  const atx::usize table_off = buf.size();
  const atx::usize table_bytes = (n_progs + 1) * sizeof(atx::u64);
  buf.resize(buf.size() + table_bytes, std::byte{0}); // placeholder table slots

  std::vector<atx::u64> offsets;
  offsets.reserve(n_progs + 1);
  offsets.push_back(static_cast<atx::u64>(buf.size())); // offsets[0] = programs region start
  for (atx::usize k = 0; k < n_progs; ++k) {
    serialize_program(buf, progs[k]);
    offsets.push_back(static_cast<atx::u64>(buf.size())); // one-past program k
  }

  // Backfill the offset table in place.
  for (atx::usize k = 0; k <= n_progs; ++k) {
    const atx::u64 o = offsets[k];
    std::memcpy(buf.data() + table_off + k * sizeof(atx::u64), &o, sizeof(atx::u64));
  }
  return buf;
}

// ===========================================================================
//  EvalInputView::parse
// ===========================================================================
Result<EvalInputView> EvalInputView::parse(InputView in) {
  const std::span<const std::byte> b = in.bytes;
  atx::u32 magic = 0;
  atx::u32 nprog32 = 0;
  atx::u32 dates32 = 0;
  atx::u32 inst32 = 0;
  atx::u32 nfields32 = 0;
  atx::u32 uni_present32 = 0;
  atx::u64 cells64 = 0;
  if (!get_pod(b, 0, magic) || magic != kEvalMagic) {
    return Err(ErrorCode::InvalidArgument, "EvalInputView: bad magic / too short");
  }
  if (!get_pod(b, 4, nprog32) || !get_pod(b, 8, dates32) || !get_pod(b, 12, inst32) ||
      !get_pod(b, 16, nfields32) || !get_pod(b, 20, uni_present32) || !get_pod(b, 24, cells64)) {
    return Err(ErrorCode::InvalidArgument, "EvalInputView: truncated header");
  }
  const atx::usize dates = dates32;
  const atx::usize instruments = inst32;
  const atx::usize n_fields = nfields32;
  const atx::usize n_programs = nprog32;

  // cells must equal dates*instruments (overflow-checked); the stored value is
  // untrusted, so we recompute and compare rather than trust it.
  atx::usize cells = 0;
  if (!checked_mul(dates, instruments, cells)) {
    return Err(ErrorCode::InvalidArgument, "EvalInputView: dates*instruments overflows");
  }
  if (cells64 != static_cast<atx::u64>(cells)) {
    return Err(ErrorCode::InvalidArgument, "EvalInputView: stored cells != dates*instruments");
  }

  // f64 column region: n_fields * cells f64, immediately after the header.
  const atx::usize cols_off = kEvalHeaderBytes;
  atx::usize col_count = 0;
  if (!checked_mul(n_fields, cells, col_count)) {
    return Err(ErrorCode::InvalidArgument, "EvalInputView: n_fields*cells overflows");
  }
  if (!f64_region_fits(b.size(), cols_off, col_count)) {
    return Err(ErrorCode::InvalidArgument, "EvalInputView: column region overruns buffer");
  }
  if (col_count != 0 && !is_aligned<atx::f64>(b.data() + cols_off)) {
    return Err(ErrorCode::InvalidArgument, "EvalInputView: column region misaligned");
  }

  // Universe mask: cells bytes (present iff the flag is set). The column byte span is
  // computed via checked_mul (spec: ALL products overflow-checked) — f64_region_fits
  // already proved it fits the buffer, but we never form the offset from an unchecked
  // product (a wrap would underflow uni_off and bypass the bounds check below).
  const bool uni_present = uni_present32 != 0;
  atx::usize col_bytes_total = 0;
  if (!checked_mul(col_count, sizeof(atx::f64), col_bytes_total)) {
    return Err(ErrorCode::InvalidArgument, "EvalInputView: column byte size overflows");
  }
  const atx::usize uni_off = cols_off + col_bytes_total; // cols_off + col_bytes_total <=
                                                         // b.size() (proved by fits) -> no wrap
  std::vector<std::uint8_t> universe;
  atx::usize cursor = uni_off;
  if (uni_present) {
    if (uni_off > b.size() || (b.size() - uni_off) < cells) {
      return Err(ErrorCode::InvalidArgument, "EvalInputView: universe mask overruns buffer");
    }
    universe.resize(cells);
    for (atx::usize i = 0; i < cells; ++i) {
      universe[i] = static_cast<std::uint8_t>(b[uni_off + i]);
    }
    cursor = uni_off + cells;
  } else {
    universe.assign(cells, std::uint8_t{1});
  }

  // Field-name dictionary: n_fields length-prefixed strings.
  std::vector<std::string> field_names;
  field_names.reserve(n_fields);
  {
    SliceReader sr{b, cursor, b.size()};
    for (atx::usize f = 0; f < n_fields; ++f) {
      std::string name;
      if (!sr.read_string(name)) {
        return Err(ErrorCode::InvalidArgument, "EvalInputView: field-name dictionary overruns");
      }
      field_names.push_back(std::move(name));
    }
    cursor = sr.pos();
  }

  // Pad to 8 -> the offset table base. (The serializer padded with zeros; we simply
  // round the cursor up, then validate the rounded offset still fits.)
  const atx::usize table_off = (cursor + 7) & ~atx::usize{7};
  const atx::usize table_entries = n_programs + 1; // n_programs is u32 -> no overflow
  if (table_off > b.size() || (b.size() - table_off) / sizeof(atx::u64) < table_entries) {
    return Err(ErrorCode::InvalidArgument, "EvalInputView: program offset table overruns buffer");
  }
  if (!is_aligned<atx::u64>(b.data() + table_off)) {
    return Err(ErrorCode::InvalidArgument, "EvalInputView: program offset table misaligned");
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto *table = reinterpret_cast<const atx::u64 *>(b.data() + table_off);
  const std::span<const atx::u64> offsets{table, table_entries};

  // Validate the table: offsets[0] starts at the end of the table, every entry is
  // monotonic non-decreasing and <= buffer size. A malformed table is rejected (no
  // OOB program slice is ever formed).
  const atx::usize data_begin = table_off + table_entries * sizeof(atx::u64);
  if (offsets[0] != static_cast<atx::u64>(data_begin)) {
    return Err(ErrorCode::InvalidArgument, "EvalInputView: first program offset misplaced");
  }
  atx::u64 prev = 0;
  for (atx::usize k = 0; k < table_entries; ++k) {
    const atx::u64 o = offsets[k];
    if (o < prev || o > static_cast<atx::u64>(b.size())) {
      return Err(ErrorCode::InvalidArgument, "EvalInputView: program offset table malformed");
    }
    prev = o;
  }

  EvalInputView v;
  v.n_programs_ = n_programs;
  v.dates_ = dates;
  v.instruments_ = instruments;
  v.n_fields_ = n_fields;
  v.cells_ = cells;
  v.universe_present_ = uni_present;
  v.columns_ = (col_count == 0) ? std::span<const atx::f64>{} : f64_alias(b, cols_off, col_count);
  v.field_names_ = std::move(field_names);
  v.universe_ = std::move(universe);
  v.prog_offsets_ = offsets;
  v.payload_ = b;
  return v;
}

// Rebuild the borrowed Panel: each field's column span ALIASES the InputView f64
// region (cells f64 starting at field f's block); names + universe are the owned
// copies. Spans are valid for the duration of the shard call (the InputView outlives
// it). create_borrowed validates the column sizes again (defence in depth).
Result<alpha::Panel> EvalInputView::panel() const {
  std::vector<std::span<const atx::f64>> cols;
  cols.reserve(n_fields_);
  for (atx::usize f = 0; f < n_fields_; ++f) {
    cols.push_back(columns_.subspan(f * cells_, cells_));
  }
  std::vector<std::string> names = field_names_; // copy (Panel takes ownership)
  std::vector<std::uint8_t> uni = universe_;     // copy (Panel owns the mask)
  return alpha::Panel::create_borrowed(dates_, instruments_, std::move(names), std::move(cols),
                                       std::move(uni));
}

// Deserialize program k from its [offsets[k], offsets[k+1]) slice, field-for-field.
Result<alpha::Program> EvalInputView::program(atx::usize k) const {
  if (k >= n_programs_) {
    return Err(ErrorCode::OutOfRange, "EvalInputView::program: index out of range");
  }
  const atx::usize begin = static_cast<atx::usize>(prog_offsets_[k]);
  const atx::usize end = static_cast<atx::usize>(prog_offsets_[k + 1]);
  // parse() proved begin <= end <= payload size; SliceReader enforces the bound.
  SliceReader sr{payload_, begin, end};

  alpha::Program prog;
  atx::u32 num_slots = 0;
  atx::u16 lookback = 0;
  atx::u32 unique_nodes = 0;
  atx::u32 total_ast = 0;
  atx::u32 peak_live = 0;
  atx::u32 cache_hits = 0;
  atx::u32 intern_attempts = 0;
  if (!sr.read_pod(num_slots) || !sr.read_pod(lookback) || !sr.read_pod(unique_nodes) ||
      !sr.read_pod(total_ast) || !sr.read_pod(peak_live) || !sr.read_pod(cache_hits) ||
      !sr.read_pod(intern_attempts)) {
    return Err(ErrorCode::InvalidArgument, "EvalInputView::program: truncated scalars");
  }
  prog.num_slots = num_slots;
  prog.required_lookback = lookback;
  prog.unique_nodes = unique_nodes;
  prog.total_ast_nodes = total_ast;
  prog.peak_live_slots = peak_live;
  prog.cache_hits = cache_hits;
  prog.intern_attempts = intern_attempts;

  // Code stream: n_code Instr, bulk-copied into an OWNED vector (memcpy has no
  // alignment requirement; we never form a span<const Instr> over the buffer).
  atx::u32 n_code = 0;
  if (!sr.read_pod(n_code)) {
    return Err(ErrorCode::InvalidArgument, "EvalInputView::program: truncated code count");
  }
  // n_code * sizeof(Instr) must fit the remaining slice (overflow-checked).
  atx::usize code_bytes = 0;
  if (!checked_mul(static_cast<atx::usize>(n_code), sizeof(alpha::Instr), code_bytes)) {
    return Err(ErrorCode::InvalidArgument, "EvalInputView::program: code byte count overflows");
  }
  if (code_bytes > sr.remaining()) {
    return Err(ErrorCode::InvalidArgument, "EvalInputView::program: code stream overruns");
  }
  prog.code.resize(n_code);
  if (!sr.read_bytes(prog.code.data(), code_bytes)) {
    return Err(ErrorCode::InvalidArgument, "EvalInputView::program: code stream short read");
  }

  // Roots: n_roots × { u32 output; length-prefixed name }.
  atx::u32 n_roots = 0;
  if (!sr.read_pod(n_roots)) {
    return Err(ErrorCode::InvalidArgument, "EvalInputView::program: truncated root count");
  }
  prog.roots.reserve(n_roots);
  for (atx::u32 r = 0; r < n_roots; ++r) {
    alpha::Program::Root root;
    if (!sr.read_pod(root.output) || !sr.read_string(root.name)) {
      return Err(ErrorCode::InvalidArgument, "EvalInputView::program: truncated root");
    }
    prog.roots.push_back(std::move(root));
  }

  // Program field dictionary: n_pfields length-prefixed strings.
  atx::u32 n_pfields = 0;
  if (!sr.read_pod(n_pfields)) {
    return Err(ErrorCode::InvalidArgument, "EvalInputView::program: truncated field count");
  }
  prog.fields.reserve(n_pfields);
  for (atx::u32 f = 0; f < n_pfields; ++f) {
    std::string name;
    if (!sr.read_string(name)) {
      return Err(ErrorCode::InvalidArgument, "EvalInputView::program: truncated field name");
    }
    prog.fields.push_back(std::move(name));
  }
  return prog;
}

// ===========================================================================
//  eval_shard — the process-portable EVAL body.
// ===========================================================================
Status eval_shard(InputView in, ShardId k, std::span<std::byte> slot) noexcept {
  Result<EvalInputView> parsed = EvalInputView::parse(in);
  if (!parsed) {
    return Err(parsed.error());
  }
  const EvalInputView &v = *parsed;
  if (k >= v.n_programs()) {
    return Err(ErrorCode::OutOfRange, "eval_shard: program id out of range");
  }

  Result<alpha::Panel> panel = v.panel();
  if (!panel) {
    return Err(panel.error());
  }
  Result<alpha::Program> prog = v.program(k);
  if (!prog) {
    return Err(prog.error());
  }

  // Run the SAME Engine::evaluate the in-process path uses — verbatim f64 columns +
  // identical universe mask -> bit-identical SignalSet. An evaluate Err propagates
  // (the submit() seam reduces to the lowest-shard-id error).
  alpha::Engine engine{*panel};
  Result<alpha::SignalSet> r = engine.evaluate(*prog);
  if (!r) {
    return Err(r.error());
  }
  const alpha::SignalSet &ss = *r;

  const atx::usize cells = v.cells();
  const atx::usize nroots = ss.alphas.size();
  // Slot must hold nroots columns of `cells` f64 (overflow-checked product).
  atx::usize need_cells = 0;
  atx::usize need_bytes = 0;
  if (!checked_mul(nroots, cells, need_cells) ||
      !checked_mul(need_cells, sizeof(atx::f64), need_bytes)) {
    return Err(ErrorCode::InvalidArgument, "eval_shard: output size overflows");
  }
  if (slot.size() < need_bytes) {
    return Err(ErrorCode::InvalidArgument, "eval_shard: slot smaller than nroots*cells f64");
  }
  // Write ONLY the verbatim f64 values, column j at offset j*cells*8. The PARENT
  // already holds the root NAMES (from the serialized programs), so names never
  // cross back. Each alpha column is exactly `cells` f64 (Engine::evaluate sizes it).
  // SAFETY: this loop runs only when nroots >= 1, and need_bytes = nroots*cells*8 was
  // overflow-checked above, so col_bytes = cells*8 <= need_bytes cannot wrap, and
  // j*col_bytes < nroots*col_bytes = need_bytes <= slot.size() stays in-bounds.
  const atx::usize col_bytes = cells * sizeof(atx::f64);
  for (atx::usize j = 0; j < nroots; ++j) {
    // ALWAYS-ON guard (not ATX_ASSERT): the memcpy below reads col_bytes == cells*8
    // from values.data(). Each Engine alpha column is `cells` f64 by construction, but
    // a debug-only assert would be ELIDED under NDEBUG and a short column would then
    // OOB-read into the slot (silent corruption), so this stays a checked Err.
    if (ss.alphas[j].values.size() != cells) {
      return Err(ErrorCode::Internal, "eval_shard: alpha column size != cells");
    }
    if (col_bytes != 0) {
      std::memcpy(slot.data() + j * col_bytes, ss.alphas[j].values.data(), col_bytes);
    }
  }
  return atx::core::Ok();
}

} // namespace atx::engine::parallel
