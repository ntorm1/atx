#include "atx/engine/parallel/workload_streams.hpp"

#include <cstddef> // std::byte
#include <cstring> // std::memcpy
#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/macro.hpp" // ATX_ASSERT
#include "atx/core/types.hpp"

#include "atx/engine/alpha/streams.hpp"     // alpha::AlphaStreams
#include "atx/engine/eval/cpcv.hpp"         // eval::CpcvFold
#include "atx/engine/parallel/executor.hpp" // InputView, ShardId
#include "atx/engine/parallel/parallel_run.hpp" // FoldResult, run_full_backtest, run_one_fold

namespace atx::engine::parallel {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Result;
using atx::core::Status;

namespace {

// ---------------------------------------------------------------------------
//  Little-endian POD writers — append a value's raw bytes to a growing buffer.
//  The wire is fixed little-endian; on the SAME binary the byte image is stable,
//  and f64 bit patterns are copied VERBATIM (no float reformatting). We do not
//  byte-swap: the parent and worker are the same binary on the same machine (the
//  §0.3 cross-endian lift is a recorded residual, not in scope here).
// ---------------------------------------------------------------------------
template <class T> void put_pod(std::vector<std::byte>& buf, const T& v) {
  const auto* p = reinterpret_cast<const std::byte*>(&v); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
  buf.insert(buf.end(), p, p + sizeof(T));
}

// Append a flat f64 array's raw bytes (verbatim bit patterns).
void put_f64_array(std::vector<std::byte>& buf, std::span<const atx::f64> a) {
  if (a.empty()) {
    return;
  }
  const auto* p = reinterpret_cast<const std::byte*>(a.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
  buf.insert(buf.end(), p, p + a.size_bytes());
}

// ---------------------------------------------------------------------------
//  Bounds-checked little-endian POD readers from a const byte span. Each returns
//  false if the field does not wholly fit (the parse views reject rather than form
//  an OOB read — same discipline as ShmSegment::open / read_manifest).
// ---------------------------------------------------------------------------
template <class T>
[[nodiscard]] bool get_pod(std::span<const std::byte> bytes, atx::usize off, T& out) noexcept {
  if (off > bytes.size() || bytes.size() - off < sizeof(T)) {
    return false;
  }
  std::memcpy(&out, bytes.data() + off, sizeof(T));
  return true;
}

// Overflow-checked usize multiply: out = a * b, false on wrap. The dimensions na/np/ni
// come from an UNTRUSTED buffer (u32 each); na*np and na*np*ni can wrap usize on a
// 64-bit target (e.g. all == u32_max), and a wrapped-small product would let a bogus
// region pass f64_region_fits and form an OOB alias. Reject the wrap instead.
[[nodiscard]] bool checked_mul(atx::usize a, atx::usize b, atx::usize& out) noexcept {
  if (a != 0 && b > (static_cast<atx::usize>(-1) / a)) {
    return false; // a * b would overflow usize
  }
  out = a * b;
  return true;
}

// Overflow-safe: does `count` f64 cells fit in [off, end)? Computes the byte span
// without ever forming an out-of-range product that wraps usize.
[[nodiscard]] bool f64_region_fits(atx::usize buf_size, atx::usize off,
                                   atx::usize count) noexcept {
  if (off > buf_size) {
    return false;
  }
  const atx::usize avail = buf_size - off;
  // count * sizeof(f64) without overflow: avail / 8 >= count.
  return count <= (avail / sizeof(atx::f64));
}

// Form a span<const f64> aliasing `bytes[off, off + count*8)`. PRECONDITION: the
// caller has already validated f64_region_fits (so this is in-bounds, no UB). The
// payload base is 8-aligned and `off` is kept a multiple of 8 by the layout, so the
// f64 alias is suitably aligned.
[[nodiscard]] std::span<const atx::f64> f64_alias(std::span<const std::byte> bytes, atx::usize off,
                                                  atx::usize count) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* p = reinterpret_cast<const atx::f64*>(bytes.data() + off);
  return std::span<const atx::f64>{p, count};
}

// Reconstruct a by-value AlphaStreams from a parse view's aliased flat arrays so the
// shard can call the EXACT in-process map primitive (run_full_backtest /
// run_one_fold) — same index math, same compute_metrics, bit-identical result. This
// copies the flat arrays into the AlphaStreams' owned vectors (the streams own their
// storage by value). The copy is per-shard but the f64 bytes are verbatim, so the
// computed FoldResult is byte-identical to the in-process path.
[[nodiscard]] atx::engine::alpha::AlphaStreams
streams_from(std::span<const atx::f64> pnl, std::span<const atx::f64> pos, atx::usize n_alphas,
             atx::usize n_periods, atx::usize n_instruments) {
  atx::engine::alpha::AlphaStreams s;
  s.n_alphas_ = n_alphas;
  s.n_periods_ = n_periods;
  s.n_instruments_ = n_instruments;
  s.pnl_flat.assign(pnl.begin(), pnl.end());
  s.pos_flat.assign(pos.begin(), pos.end());
  return s;
}

// Write a POD FoldResult into a shard's output slot. PRECONDITION (validated by the
// caller): slot.size() >= sizeof(FoldResult). FoldResult is trivially copyable
// (static_assert in parallel_run.hpp), so the raw memcpy is defined.
void write_fold_result(std::span<std::byte> slot, const FoldResult& r) noexcept {
  std::memcpy(slot.data(), &r, sizeof(r));
}

} // namespace

// ===========================================================================
//  serialize_backtests_input
// ===========================================================================
std::vector<std::byte> serialize_backtests_input(const atx::engine::alpha::AlphaStreams& streams,
                                                 atx::f64 book_size) {
  const atx::usize na = streams.n_alphas();
  const atx::usize np = streams.n_periods();
  const atx::usize ni = streams.n_instruments();

  std::vector<std::byte> buf;
  buf.reserve(kBacktestsHeaderBytes + streams.pnl_flat.size() * sizeof(atx::f64) +
              streams.pos_flat.size() * sizeof(atx::f64));

  // Header (24 bytes, multiple of 8 -> the f64 arrays land 8-aligned).
  put_pod(buf, kBacktestsMagic);
  put_pod(buf, static_cast<atx::u32>(na));
  put_pod(buf, static_cast<atx::u32>(np));
  put_pod(buf, static_cast<atx::u32>(ni));
  put_pod(buf, book_size); // f64 at offset 16 (8-aligned)
  ATX_ASSERT(buf.size() == kBacktestsHeaderBytes);

  // pnl_flat then pos_flat, raw f64 bytes (verbatim).
  put_f64_array(buf, streams.pnl_flat);
  put_f64_array(buf, streams.pos_flat);
  return buf;
}

// ===========================================================================
//  serialize_cpcv_input
// ===========================================================================
std::vector<std::byte> serialize_cpcv_input(const atx::engine::alpha::AlphaStreams& streams,
                                            atx::usize alpha_id, atx::f64 book_size,
                                            std::span<const atx::engine::eval::CpcvFold> folds) {
  const atx::usize na = streams.n_alphas();
  const atx::usize np = streams.n_periods();
  const atx::usize ni = streams.n_instruments();
  const atx::usize nf = folds.size();

  std::vector<std::byte> buf;

  // Header (32 bytes, multiple of 8).
  put_pod(buf, kCpcvMagic);
  put_pod(buf, static_cast<atx::u32>(na));
  put_pod(buf, static_cast<atx::u32>(np));
  put_pod(buf, static_cast<atx::u32>(ni));
  put_pod(buf, static_cast<atx::u32>(alpha_id));
  put_pod(buf, static_cast<atx::u32>(nf));
  put_pod(buf, book_size); // f64 at offset 24 (8-aligned)
  ATX_ASSERT(buf.size() == kCpcvHeaderBytes);

  // The two f64 arrays (both 8-aligned; pos_flat ends 8-aligned so the u64 table
  // below is 8-aligned too).
  put_f64_array(buf, streams.pnl_flat);
  put_f64_array(buf, streams.pos_flat);

  // Fold offset table: (nf + 1) u64 byte-offsets (from payload start). entry[f] is
  // where fold f's u64 test_idx run begins; entry[nf] is one-past the last fold.
  const atx::usize table_off = buf.size();
  const atx::usize table_bytes = (nf + 1) * sizeof(atx::u64);
  atx::usize cursor = table_off + table_bytes; // first fold's data begins after the table
  std::vector<atx::u64> offsets;
  offsets.reserve(nf + 1);
  for (atx::usize f = 0; f < nf; ++f) {
    offsets.push_back(static_cast<atx::u64>(cursor));
    cursor += folds[f].test_idx.size() * sizeof(atx::u64);
  }
  offsets.push_back(static_cast<atx::u64>(cursor)); // sentinel end
  for (const atx::u64 o : offsets) {
    put_pod(buf, o);
  }
  ATX_ASSERT(buf.size() == table_off + table_bytes);

  // Fold data: each fold's test_idx as contiguous u64 period indices.
  for (atx::usize f = 0; f < nf; ++f) {
    for (const atx::usize t : folds[f].test_idx) {
      put_pod(buf, static_cast<atx::u64>(t));
    }
  }
  return buf;
}

// ===========================================================================
//  BacktestsInputView::parse
// ===========================================================================
Result<BacktestsInputView> BacktestsInputView::parse(InputView in) noexcept {
  const std::span<const std::byte> b = in.bytes;
  atx::u32 magic = 0;
  atx::u32 na32 = 0;
  atx::u32 np32 = 0;
  atx::u32 ni32 = 0;
  atx::f64 book = 0.0;
  if (!get_pod(b, 0, magic) || magic != kBacktestsMagic) {
    return Err(ErrorCode::InvalidArgument, "BacktestsInputView: bad magic / too short");
  }
  if (!get_pod(b, 4, na32) || !get_pod(b, 8, np32) || !get_pod(b, 12, ni32) ||
      !get_pod(b, 16, book)) {
    return Err(ErrorCode::InvalidArgument, "BacktestsInputView: truncated header");
  }
  const atx::usize na = na32;
  const atx::usize np = np32;
  const atx::usize ni = ni32;

  // The two f64 regions, validated against the buffer length before aliasing. The
  // cell COUNTS are overflow-checked products of untrusted u32 dimensions (a wrapped
  // product would let an OOB region pass f64_region_fits — reviewer finding).
  const atx::usize pnl_off = kBacktestsHeaderBytes;
  atx::usize pnl_count = 0;
  atx::usize pos_count = 0;
  if (!checked_mul(na, np, pnl_count) || !checked_mul(pnl_count, ni, pos_count)) {
    return Err(ErrorCode::InvalidArgument, "BacktestsInputView: dimension product overflows");
  }
  if (!f64_region_fits(b.size(), pnl_off, pnl_count)) {
    return Err(ErrorCode::InvalidArgument, "BacktestsInputView: pnl region overruns buffer");
  }
  // pos_off = pnl_off + pnl_count*8; f64_region_fits already proved pnl_count*8 fits
  // [pnl_off, buf), so this sum is <= b.size() and cannot wrap.
  const atx::usize pos_off = pnl_off + pnl_count * sizeof(atx::f64);
  if (!f64_region_fits(b.size(), pos_off, pos_count)) {
    return Err(ErrorCode::InvalidArgument, "BacktestsInputView: pos region overruns buffer");
  }

  BacktestsInputView v;
  v.n_alphas_ = na;
  v.n_periods_ = np;
  v.n_instruments_ = ni;
  v.book_size_ = book;
  v.pnl_ = f64_alias(b, pnl_off, pnl_count);
  v.pos_ = f64_alias(b, pos_off, pos_count);
  return v;
}

std::span<const atx::f64> BacktestsInputView::pnl_of(atx::usize alpha) const noexcept {
  ATX_ASSERT(alpha < n_alphas_);
  return pnl_.subspan(alpha * n_periods_, n_periods_);
}

std::span<const atx::f64> BacktestsInputView::positions_of(atx::usize alpha,
                                                          atx::usize period) const noexcept {
  ATX_ASSERT(alpha < n_alphas_);
  ATX_ASSERT(period < n_periods_);
  const atx::usize off = (alpha * n_periods_ + period) * n_instruments_;
  return pos_.subspan(off, n_instruments_);
}

// ===========================================================================
//  CpcvInputView::parse
// ===========================================================================
Result<CpcvInputView> CpcvInputView::parse(InputView in) noexcept {
  const std::span<const std::byte> b = in.bytes;
  atx::u32 magic = 0;
  atx::u32 na32 = 0;
  atx::u32 np32 = 0;
  atx::u32 ni32 = 0;
  atx::u32 aid32 = 0;
  atx::u32 nf32 = 0;
  atx::f64 book = 0.0;
  if (!get_pod(b, 0, magic) || magic != kCpcvMagic) {
    return Err(ErrorCode::InvalidArgument, "CpcvInputView: bad magic / too short");
  }
  if (!get_pod(b, 4, na32) || !get_pod(b, 8, np32) || !get_pod(b, 12, ni32) ||
      !get_pod(b, 16, aid32) || !get_pod(b, 20, nf32) || !get_pod(b, 24, book)) {
    return Err(ErrorCode::InvalidArgument, "CpcvInputView: truncated header");
  }
  const atx::usize na = na32;
  const atx::usize np = np32;
  const atx::usize ni = ni32;
  const atx::usize nf = nf32;

  const atx::usize pnl_off = kCpcvHeaderBytes;
  atx::usize pnl_count = 0;
  atx::usize pos_count = 0;
  if (!checked_mul(na, np, pnl_count) || !checked_mul(pnl_count, ni, pos_count)) {
    return Err(ErrorCode::InvalidArgument, "CpcvInputView: dimension product overflows");
  }
  if (!f64_region_fits(b.size(), pnl_off, pnl_count)) {
    return Err(ErrorCode::InvalidArgument, "CpcvInputView: pnl region overruns buffer");
  }
  const atx::usize pos_off = pnl_off + pnl_count * sizeof(atx::f64);
  if (!f64_region_fits(b.size(), pos_off, pos_count)) {
    return Err(ErrorCode::InvalidArgument, "CpcvInputView: pos region overruns buffer");
  }

  // Fold offset table: (nf + 1) u64 entries, immediately after pos_flat (8-aligned).
  const atx::usize table_off = pos_off + pos_count * sizeof(atx::f64);
  // (nf + 1) u64s must fit. nf is a u32 so nf + 1 cannot overflow usize.
  if (table_off > b.size() || (b.size() - table_off) / sizeof(atx::u64) < (nf + 1)) {
    return Err(ErrorCode::InvalidArgument, "CpcvInputView: fold offset table overruns buffer");
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* table = reinterpret_cast<const atx::u64*>(b.data() + table_off);
  const std::span<const atx::u64> offsets{table, nf + 1};

  // Validate the table: every entry monotonic non-decreasing, in-bounds, and on an
  // 8-byte boundary; the first entry must start at the end of the table; the last
  // (sentinel) must not exceed the buffer. A malformed table is rejected (no OOB).
  const atx::usize data_begin = table_off + (nf + 1) * sizeof(atx::u64);
  atx::u64 prev = static_cast<atx::u64>(data_begin);
  if (nf > 0 && offsets[0] != prev) {
    return Err(ErrorCode::InvalidArgument, "CpcvInputView: first fold offset misplaced");
  }
  for (atx::usize f = 0; f <= nf; ++f) {
    const atx::u64 o = offsets[f];
    if (o < prev || o > static_cast<atx::u64>(b.size()) || (o % sizeof(atx::u64)) != 0) {
      return Err(ErrorCode::InvalidArgument, "CpcvInputView: fold offset table malformed");
    }
    prev = o;
  }

  CpcvInputView v;
  v.n_alphas_ = na;
  v.n_periods_ = np;
  v.n_instruments_ = ni;
  v.alpha_id_ = aid32;
  v.n_folds_ = nf;
  v.book_size_ = book;
  v.pnl_ = f64_alias(b, pnl_off, pnl_count);
  v.pos_ = f64_alias(b, pos_off, pos_count);
  v.fold_offsets_ = offsets;
  v.payload_ = b;
  return v;
}

std::span<const atx::f64> CpcvInputView::pnl_of(atx::usize alpha) const noexcept {
  ATX_ASSERT(alpha < n_alphas_);
  return pnl_.subspan(alpha * n_periods_, n_periods_);
}

std::span<const atx::f64> CpcvInputView::positions_of(atx::usize alpha,
                                                     atx::usize period) const noexcept {
  ATX_ASSERT(alpha < n_alphas_);
  ATX_ASSERT(period < n_periods_);
  const atx::usize off = (alpha * n_periods_ + period) * n_instruments_;
  return pos_.subspan(off, n_instruments_);
}

std::span<const atx::u64> CpcvInputView::fold_test_idx(atx::usize f) const noexcept {
  ATX_ASSERT(f + 1 < fold_offsets_.size()); // f < n_folds (offsets has n_folds+1 entries)
  const atx::usize begin = static_cast<atx::usize>(fold_offsets_[f]);
  const atx::usize end = static_cast<atx::usize>(fold_offsets_[f + 1]);
  // SAFETY: parse() validated begin <= end <= payload size and both 8-aligned, so
  // the [begin, end) range holds (end - begin)/8 whole u64 indices, in-bounds.
  const atx::usize count = (end - begin) / sizeof(atx::u64);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* p = reinterpret_cast<const atx::u64*>(payload_.data() + begin);
  return std::span<const atx::u64>{p, count};
}

// ===========================================================================
//  backtests_shard — the process-portable BACKTESTS body.
// ===========================================================================
Status backtests_shard(InputView in, ShardId a, std::span<std::byte> slot) noexcept {
  if (slot.size() < sizeof(FoldResult)) {
    return Err(ErrorCode::InvalidArgument, "backtests_shard: slot smaller than FoldResult");
  }
  Result<BacktestsInputView> parsed = BacktestsInputView::parse(in);
  if (!parsed) {
    return Err(parsed.error());
  }
  const BacktestsInputView& v = *parsed;
  if (a >= v.n_alphas()) {
    return Err(ErrorCode::OutOfRange, "backtests_shard: alpha id out of range");
  }
  // Reconstruct the streams and run the SAME full-sample metric the in-process path
  // uses — byte-identical FoldResult (verbatim f64 bytes -> identical compute_metrics).
  const atx::engine::alpha::AlphaStreams streams =
      streams_from(v.pnl(), v.pos(), v.n_alphas(), v.n_periods(), v.n_instruments());
  const FoldResult r = run_full_backtest(streams, a, v.book_size());
  write_fold_result(slot, r);
  return atx::core::Ok();
}

// ===========================================================================
//  cpcv_shard — the process-portable CPCV body.
// ===========================================================================
Status cpcv_shard(InputView in, ShardId f, std::span<std::byte> slot) noexcept {
  if (slot.size() < sizeof(FoldResult)) {
    return Err(ErrorCode::InvalidArgument, "cpcv_shard: slot smaller than FoldResult");
  }
  Result<CpcvInputView> parsed = CpcvInputView::parse(in);
  if (!parsed) {
    return Err(parsed.error());
  }
  const CpcvInputView& v = *parsed;
  if (f >= v.n_folds()) {
    return Err(ErrorCode::OutOfRange, "cpcv_shard: fold id out of range");
  }
  const atx::usize alpha_id = v.alpha_id();
  if (alpha_id >= v.n_alphas()) {
    return Err(ErrorCode::OutOfRange, "cpcv_shard: alpha id out of range");
  }
  // Rebuild fold f's test_idx (validate every period index < n_periods so run_one_fold
  // never indexes the streams OOB — the parse view's bytes are untrusted SHM data).
  const std::span<const atx::u64> test = v.fold_test_idx(f);
  atx::engine::eval::CpcvFold fold;
  fold.test_idx.reserve(test.size());
  for (const atx::u64 t : test) {
    if (t >= static_cast<atx::u64>(v.n_periods())) {
      return Err(ErrorCode::OutOfRange, "cpcv_shard: test index out of range");
    }
    fold.test_idx.push_back(static_cast<atx::usize>(t));
  }
  const atx::engine::alpha::AlphaStreams streams =
      streams_from(v.pnl(), v.pos(), v.n_alphas(), v.n_periods(), v.n_instruments());
  const FoldResult r = run_one_fold(streams, alpha_id, f, fold, v.book_size());
  write_fold_result(slot, r);
  return atx::core::Ok();
}

} // namespace atx::engine::parallel
