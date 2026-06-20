#include "atx/engine/parallel/workload_mine.hpp"

#include <cstddef>  // std::byte
#include <cstdint>  // std::uint8_t, std::uintptr_t
#include <cstring>  // std::memcpy
#include <optional> // std::optional (worker-side corr index)
#include <span>
#include <string>
#include <string_view> // std::string_view (op-name dictionary)
#include <utility>     // std::move
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/macro.hpp" // ATX_ASSERT
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"  // alpha::compile, alpha::Program
#include "atx/engine/alpha/panel.hpp"     // alpha::Panel, alpha::SignalSet
#include "atx/engine/alpha/parser.hpp"    // alpha::Ast, alpha::Expr, alpha::ExprId, kNoExpr
#include "atx/engine/alpha/registry.hpp"  // alpha::Library, alpha::OpSig
#include "atx/engine/alpha/streams.hpp"   // alpha::extract_streams, AlphaStreams
#include "atx/engine/alpha/typecheck.hpp" // alpha::analyze, alpha::Analysis
#include "atx/engine/alpha/vm.hpp"        // alpha::Engine

#include "atx/engine/combine/correlation.hpp" // combine::pairwise_complete_corr (worst_corr)
#include "atx/engine/combine/store.hpp"       // combine::AlphaId (corr index keys)

#include "atx/engine/factory/pool_view.hpp" // factory::PoolView (worker-side snapshot backing)

#include "atx/engine/exec/execution_sim.hpp" // exec::ExecutionSimulator, CommissionCfg/Mode

#include "atx/engine/factory/fitness.hpp" // factory::pool_aware_fitness, FitnessReport
#include "atx/engine/factory/genome.hpp"  // factory::Genome

#include "atx/engine/library/corr_index.hpp" // library::CorrNeighborIndex (SimHash, byte-identical)

#include "atx/engine/loop/weight_policy.hpp" // engine::WeightPolicy, Transform

#include "atx/engine/parallel/executor.hpp"      // InputView, ShardId
#include "atx/engine/parallel/workload_eval.hpp" // serialize_eval_input / EvalInputView (panel reuse)

namespace atx::engine::parallel {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Result;
using atx::core::Status;

namespace alpha = atx::engine::alpha;
namespace factory = atx::engine::factory;
namespace library = atx::engine::library;
namespace exec = atx::engine::exec;

namespace {

// ---------------------------------------------------------------------------
//  Little-endian POD / array writers — append raw bytes to a growing buffer. The
//  wire is fixed little-endian; f64 bit patterns are copied VERBATIM (no float
//  reformatting). No byte-swap: parent and worker are the same binary on the same
//  machine (the cross-endian lift is a recorded residual). Mirrors workload_eval.cpp.
// ---------------------------------------------------------------------------
template <class T> void put_pod(std::vector<std::byte> &buf, const T &v) {
  const auto *p = reinterpret_cast<const std::byte *>(
      &v); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
  buf.insert(buf.end(), p, p + sizeof(T));
}

void put_f64_array(std::vector<std::byte> &buf, std::span<const atx::f64> a) {
  if (a.empty()) {
    return;
  }
  const auto *p = reinterpret_cast<const std::byte *>(
      a.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
  buf.insert(buf.end(), p, p + a.size_bytes());
}

void put_string(std::vector<std::byte> &buf, std::string_view s) {
  ATX_ASSERT(s.size() <= static_cast<atx::usize>(0xFFFFFFFFU)); // trusted (in-grammar names)
  put_pod(buf, static_cast<atx::u32>(s.size()));
  const auto *p = reinterpret_cast<const std::byte *>(
      s.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
  buf.insert(buf.end(), p, p + s.size());
}

void put_bytes(std::vector<std::byte> &buf, std::span<const std::byte> b) {
  buf.insert(buf.end(), b.begin(), b.end());
}

// Pad `buf` up to the next multiple of 8 with zero bytes (so a trailing u64/f64
// array lands 8-aligned within the 8-aligned payload).
void pad_to_8(std::vector<std::byte> &buf) {
  while ((buf.size() % 8) != 0) {
    buf.push_back(std::byte{0});
  }
}

// ---------------------------------------------------------------------------
//  Bounds-checked little-endian readers / overflow guards (workload_eval idioms).
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
// an UNTRUSTED buffer; a wrapped-small product would let a bogus region pass a
// fits-check and form an OOB alias. Reject the wrap instead.
[[nodiscard]] bool checked_mul(atx::usize a, atx::usize b, atx::usize &out) noexcept {
  if (a != 0 && b > (static_cast<atx::usize>(-1) / a)) {
    return false;
  }
  out = a * b;
  return true;
}

// Overflow-safe: does `count` f64 cells fit in [off, buf_size)?
[[nodiscard]] bool f64_region_fits(atx::usize buf_size, atx::usize off, atx::usize count) noexcept {
  if (off > buf_size) {
    return false;
  }
  const atx::usize avail = buf_size - off;
  return count <= (avail / sizeof(atx::f64));
}

// Form a span<const f64> aliasing `bytes[off, off + count*8)`. PRECONDITION: the
// caller has already validated f64_region_fits AND 8-alignment of the base ptr.
[[nodiscard]] std::span<const atx::f64> f64_alias(std::span<const std::byte> bytes, atx::usize off,
                                                  atx::usize count) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto *p = reinterpret_cast<const atx::f64 *>(bytes.data() + off);
  return std::span<const atx::f64>{p, count};
}

// True iff `ptr` is suitably aligned for type `T`. Untrusted SHM bytes could put a
// region at a misaligned offset; forming a span<const T> over it is UB -> reject.
template <class T> [[nodiscard]] bool is_aligned(const void *ptr) noexcept {
  return (reinterpret_cast<std::uintptr_t>(ptr) % alignof(T)) ==
         0; // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

// ---------------------------------------------------------------------------
//  Bounded forward cursor over a const byte slice (workload_eval's SliceReader).
//  Each read validates the field fits the REMAINING slice; a short read sets
//  ok=false and the caller bails with Err (no OOB on untrusted genome bytes).
// ---------------------------------------------------------------------------
class SliceReader {
public:
  SliceReader(std::span<const std::byte> bytes, atx::usize begin, atx::usize end) noexcept
      : bytes_{bytes}, pos_{begin}, end_{end} {
    ATX_ASSERT(begin <= end && end <= bytes.size()); // caller-validated bounds
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

private:
  std::span<const std::byte> bytes_;
  atx::usize pos_{};
  atx::usize end_{};
  bool ok_{true};
};

// ---------------------------------------------------------------------------
//  Genome wire serialization (Ast arena field-for-field; op-name dictionary).
// ---------------------------------------------------------------------------

// Serialize one genome's Ast into `buf`. Each Call node's OpSig* is replaced by an
// index into a per-genome op-name dictionary (built here in first-seen node order),
// so the worker remaps name -> lib.find(name). canon_hash is carried verbatim.
void serialize_genome(std::vector<std::byte> &buf, const factory::Genome &g) {
  const alpha::Ast &ast = g.ast;
  const std::span<const alpha::Expr> nodes = ast.nodes();

  // Build the op-name dictionary (deterministic: first-seen order over the arena).
  std::vector<std::string_view> op_names;
  auto op_name_index = [&op_names](std::string_view name) -> atx::u32 {
    for (atx::usize i = 0; i < op_names.size(); ++i) {
      if (op_names[i] == name) {
        return static_cast<atx::u32>(i);
      }
    }
    op_names.push_back(name);
    return static_cast<atx::u32>(op_names.size() - 1);
  };

  put_pod(buf, g.canon_hash);

  // String pool (Ast field/identifier names), in id order.
  // ExprId name_id indexes this pool; we serialize every interned string so the
  // worker rebuilds the pool with identical ids.
  std::vector<std::string_view> strings;
  // The Ast exposes field_name(id) but not a string count; collect via the max
  // name_id referenced + 1 is unsafe (gaps), so reconstruct by scanning nodes for the
  // highest referenced id and emit [0, max+1). A name_id is only meaningful on
  // Field/Member nodes; intern() assigns ids densely from 0, so the referenced set is
  // a prefix — but to be robust we emit up to the max referenced id inclusive.
  atx::u32 max_name_id = 0;
  bool any_name = false;
  for (const alpha::Expr &e : nodes) {
    if (e.kind == alpha::Expr::Kind::Field || e.kind == alpha::Expr::Kind::Member) {
      any_name = true;
      if (e.name_id > max_name_id) {
        max_name_id = e.name_id;
      }
    }
  }
  const atx::u32 n_strings = any_name ? (max_name_id + 1U) : 0U;
  put_pod(buf, n_strings);
  for (atx::u32 i = 0; i < n_strings; ++i) {
    put_string(buf, ast.field_name(i));
  }

  // Nodes: a fixed wire record per Expr.
  ATX_ASSERT(nodes.size() <= static_cast<atx::usize>(0xFFFFFFFFU));
  put_pod(buf, static_cast<atx::u32>(nodes.size()));
  for (const alpha::Expr &e : nodes) {
    put_pod(buf, static_cast<atx::u8>(e.kind));
    put_pod(buf, static_cast<atx::u8>(e.dollar ? 1U : 0U));
    put_pod(buf, static_cast<atx::u8>(e.opcode));
    put_pod(buf, e.n_hparams);
    put_pod(buf, e.name_id);
    put_pod(buf, e.a);
    put_pod(buf, e.b);
    put_pod(buf, e.c);
    const atx::u32 op_idx = (e.kind == alpha::Expr::Kind::Call && e.op != nullptr)
                                ? op_name_index(e.op->name)
                                : 0xFFFFFFFFU; // sentinel: not a Call / no op
    put_pod(buf, op_idx);
    put_pod(buf, e.value);
    put_pod(buf, e.hparams[0]);
    put_pod(buf, e.hparams[1]);
  }

  // The single root ExprId (genomes carry one root).
  const alpha::ExprId root = ast.roots().front().root;
  put_pod(buf, root);

  // Op-name dictionary.
  ATX_ASSERT(op_names.size() <= static_cast<atx::usize>(0xFFFFFFFFU));
  put_pod(buf, static_cast<atx::u32>(op_names.size()));
  for (const std::string_view name : op_names) {
    put_string(buf, name);
  }
}

} // namespace

// ===========================================================================
//  serialize_mine_input
// ===========================================================================
std::vector<std::byte> serialize_mine_input(std::span<const factory::Genome> genomes,
                                            const MineWorkItem &pool, const alpha::Panel &panel,
                                            const factory::FitnessCfg &fit_cfg,
                                            const atx::engine::WeightPolicy &policy,
                                            const exec::ExecutionSimulator &sim) {
  const atx::usize n_genomes = genomes.size();
  const atx::usize n_periods = pool.n_periods;
  const atx::usize pool_n_alphas = pool.pool_n_alphas;

  // Trusted (programmer-side) input: dimensions must fit u32 for the fixed header.
  ATX_ASSERT(n_genomes <= static_cast<atx::usize>(0xFFFFFFFFU));
  ATX_ASSERT(n_periods <= static_cast<atx::usize>(0xFFFFFFFFU));
  ATX_ASSERT(pool_n_alphas <= static_cast<atx::usize>(0xFFFFFFFFU));

  const exec::CommissionCfg &comm = sim.commission_cfg();
  const atx::u32 wp_flags = (policy.industry_neutral ? 1U : 0U) | (policy.dollar_neutral ? 2U : 0U);

  std::vector<std::byte> buf;

  // Header (104 bytes, multiple of 8 -> trailing f64 arrays land 8-aligned). The u32
  // block (12 * u32 == 48 bytes) precedes the f64 block (7 * f64 == 56 bytes); the f64
  // block thus begins at offset 48 (8-aligned).
  put_pod(buf, kMineMagic);
  put_pod(buf, static_cast<atx::u32>(n_genomes));
  put_pod(buf, static_cast<atx::u32>(n_periods));
  put_pod(buf, static_cast<atx::u32>(pool_n_alphas));
  put_pod(buf, static_cast<atx::u32>(fit_cfg.trial_count));
  put_pod(buf, static_cast<atx::u32>(fit_cfg.cpcv.n_groups));
  put_pod(buf, static_cast<atx::u32>(fit_cfg.cpcv.n_test_groups));
  put_pod(buf, static_cast<atx::u32>(policy.transform));
  put_pod(buf, wp_flags);
  put_pod(buf, static_cast<atx::u32>(comm.mode));
  put_pod(buf, static_cast<atx::u32>(0U)); // pad0
  put_pod(buf, static_cast<atx::u32>(0U)); // pad1
  // The f64 block (offsets 48..104). pool_seed is carried as raw u64 bits in an f64 slot.
  atx::f64 pool_seed_bits = 0.0;
  std::memcpy(&pool_seed_bits, &pool.pool_seed, sizeof(atx::u64)); // verbatim seed bits
  put_pod(buf, pool_seed_bits);
  put_pod(buf, fit_cfg.book_size);
  put_pod(buf, fit_cfg.cpcv.embargo);
  put_pod(buf, policy.gross_leverage);
  put_pod(buf, policy.truncation);
  put_pod(buf, policy.winsorize_limit);
  put_pod(buf, comm.per_dollar_bps);
  ATX_ASSERT(buf.size() == kMineHeaderBytes);

  // Pool snapshot pnl: pool_n_alphas * n_periods f64, alpha-major (verbatim). 8-aligned.
  put_f64_array(buf, pool.pool_pnl_flat);

  // Embedded panel blob: a panel-only serialize_eval_input buffer (REUSE S7.5c). Pad to
  // 8 then a u64 length prefix then the blob bytes (blob byte 0 lands 8-aligned).
  pad_to_8(buf);
  const std::vector<std::byte> panel_blob =
      serialize_eval_input(std::span<const alpha::Program>{}, panel);
  put_pod(buf, static_cast<atx::u64>(panel_blob.size()));
  put_bytes(buf, std::span<const std::byte>{panel_blob});

  // Pad to 8 so the genome offset table (u64) is 8-aligned.
  pad_to_8(buf);

  // Genome offset table: (n_genomes + 1) u64 byte-offsets FROM PAYLOAD START. We do
  // not know per-genome sizes until serialized; reserve the table slots, serialize the
  // genomes into the buffer, then backfill the table.
  const atx::usize table_off = buf.size();
  const atx::usize table_bytes = (n_genomes + 1) * sizeof(atx::u64);
  buf.resize(buf.size() + table_bytes, std::byte{0}); // placeholder table slots

  std::vector<atx::u64> offsets;
  offsets.reserve(n_genomes + 1);
  offsets.push_back(static_cast<atx::u64>(buf.size())); // offsets[0] = genome region start
  for (atx::usize k = 0; k < n_genomes; ++k) {
    serialize_genome(buf, genomes[k]);
    offsets.push_back(static_cast<atx::u64>(buf.size())); // one-past genome k
  }
  for (atx::usize k = 0; k <= n_genomes; ++k) {
    const atx::u64 o = offsets[k];
    std::memcpy(buf.data() + table_off + k * sizeof(atx::u64), &o, sizeof(atx::u64));
  }
  return buf;
}

// ===========================================================================
//  MineInputView::parse
// ===========================================================================
Result<MineInputView> MineInputView::parse(InputView in) {
  const std::span<const std::byte> b = in.bytes;

  atx::u32 magic = 0;
  if (!get_pod(b, 0, magic) || magic != kMineMagic) {
    return Err(ErrorCode::InvalidArgument, "MineInputView: bad magic / too short");
  }
  // Header: 12 u32 then 7 f64. Read each at its fixed offset (all validated by get_pod).
  atx::u32 ngen32 = 0, nper32 = 0, npool32 = 0, trial32 = 0;
  atx::u32 cg32 = 0, ctg32 = 0, wtransform32 = 0, wflags32 = 0, cmode32 = 0, pad0 = 0, pad1 = 0;
  if (!get_pod(b, 4, ngen32) || !get_pod(b, 8, nper32) || !get_pod(b, 12, npool32) ||
      !get_pod(b, 16, trial32) || !get_pod(b, 20, cg32) || !get_pod(b, 24, ctg32) ||
      !get_pod(b, 28, wtransform32) || !get_pod(b, 32, wflags32) || !get_pod(b, 36, cmode32) ||
      !get_pod(b, 40, pad0) || !get_pod(b, 44, pad1)) {
    return Err(ErrorCode::InvalidArgument, "MineInputView: truncated u32 header");
  }
  atx::f64 pool_seed_bits = 0.0, book = 0.0, emb = 0.0, gl = 0.0, trunc = 0.0, wl = 0.0, pdb = 0.0;
  if (!get_pod(b, 48, pool_seed_bits) || !get_pod(b, 56, book) || !get_pod(b, 64, emb) ||
      !get_pod(b, 72, gl) || !get_pod(b, 80, trunc) || !get_pod(b, 88, wl) ||
      !get_pod(b, 96, pdb)) {
    return Err(ErrorCode::InvalidArgument, "MineInputView: truncated f64 header");
  }
  (void)pad0;
  (void)pad1;

  const atx::usize n_genomes = ngen32;
  const atx::usize n_periods = nper32;
  const atx::usize pool_n_alphas = npool32;

  // Pool snapshot pnl region: pool_n_alphas * n_periods f64, immediately after the header.
  const atx::usize pool_off = kMineHeaderBytes;
  atx::usize pool_count = 0;
  if (!checked_mul(pool_n_alphas, n_periods, pool_count)) {
    return Err(ErrorCode::InvalidArgument, "MineInputView: pool_n_alphas*n_periods overflows");
  }
  if (!f64_region_fits(b.size(), pool_off, pool_count)) {
    return Err(ErrorCode::InvalidArgument, "MineInputView: pool snapshot overruns buffer");
  }
  if (pool_count != 0 && !is_aligned<atx::f64>(b.data() + pool_off)) {
    return Err(ErrorCode::InvalidArgument, "MineInputView: pool snapshot misaligned");
  }
  atx::usize pool_bytes = 0;
  if (!checked_mul(pool_count, sizeof(atx::f64), pool_bytes)) {
    return Err(ErrorCode::InvalidArgument, "MineInputView: pool snapshot byte size overflows");
  }
  // Pad pool end up to 8 -> the panel blob's u64 length prefix.
  atx::usize cursor = pool_off + pool_bytes; // <= b.size() (proved by fits) -> no wrap
  cursor = (cursor + 7) & ~atx::usize{7};

  // Panel blob: u64 length prefix then `len` bytes. get_pod proved cursor + 8 <= b.size(),
  // so blob_off (= cursor + 8) <= b.size() (no wrap). Reject a length that cannot fit the
  // buffer at all BEFORE the subspan (a >b.size() len on a 32-bit usize would truncate; we
  // reject it outright), then the fits-check below proves blob_off + blob_len <= b.size().
  atx::u64 blob_len64 = 0;
  if (!get_pod(b, cursor, blob_len64)) {
    return Err(ErrorCode::InvalidArgument, "MineInputView: truncated panel blob length");
  }
  if (blob_len64 > static_cast<atx::u64>(b.size())) {
    return Err(ErrorCode::InvalidArgument, "MineInputView: panel blob length exceeds buffer");
  }
  const atx::usize blob_off = cursor + sizeof(atx::u64); // <= b.size() (get_pod proved)
  const atx::usize blob_len = static_cast<atx::usize>(blob_len64);
  if (blob_off > b.size() || (b.size() - blob_off) < blob_len) {
    return Err(ErrorCode::InvalidArgument, "MineInputView: panel blob overruns buffer");
  }
  const std::span<const std::byte> panel_blob = b.subspan(blob_off, blob_len);
  cursor = blob_off + blob_len; // <= b.size() (proved by the fits-check above) -> no wrap

  // Pad to 8 -> the genome offset table base.
  const atx::usize table_off = (cursor + 7) & ~atx::usize{7};
  const atx::usize table_entries = n_genomes + 1; // n_genomes is u32 -> no overflow
  if (table_off > b.size() || (b.size() - table_off) / sizeof(atx::u64) < table_entries) {
    return Err(ErrorCode::InvalidArgument, "MineInputView: genome offset table overruns buffer");
  }
  if (!is_aligned<atx::u64>(b.data() + table_off)) {
    return Err(ErrorCode::InvalidArgument, "MineInputView: genome offset table misaligned");
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto *table = reinterpret_cast<const atx::u64 *>(b.data() + table_off);
  std::vector<atx::u64> offsets(table, table + table_entries); // owned copy (validated below)

  // Validate the table: offsets[0] is the byte right after the table; every entry is
  // monotonic non-decreasing and <= buffer size (a malformed table is rejected — no OOB
  // genome slice is ever formed).
  // table_entries*8 <= b.size()-table_off was just proved by the division-form check above,
  // so this product cannot wrap; recompute it through checked_mul so the safety is self-evident.
  atx::usize table_bytes = 0;
  if (!checked_mul(table_entries, sizeof(atx::u64), table_bytes)) {
    return Err(ErrorCode::InvalidArgument, "MineInputView: genome offset table size overflows");
  }
  const atx::usize data_begin = table_off + table_bytes;
  if (offsets[0] != static_cast<atx::u64>(data_begin)) {
    return Err(ErrorCode::InvalidArgument, "MineInputView: first genome offset misplaced");
  }
  atx::u64 prev = 0;
  for (atx::usize k = 0; k < table_entries; ++k) {
    const atx::u64 o = offsets[k];
    if (o < prev || o > static_cast<atx::u64>(b.size())) {
      return Err(ErrorCode::InvalidArgument, "MineInputView: genome offset table malformed");
    }
    prev = o;
  }

  MineInputView v;
  v.n_genomes_ = n_genomes;
  v.n_periods_ = n_periods;
  v.pool_n_alphas_ = pool_n_alphas;
  std::memcpy(&v.pool_seed_, &pool_seed_bits, sizeof(atx::u64)); // recover the u64 seed bits
  v.trial_count_ = trial32;
  v.cpcv_n_groups_ = cg32;
  v.cpcv_n_test_groups_ = ctg32;
  v.wp_transform_ = wtransform32;
  v.wp_flags_ = wflags32;
  v.commission_mode_ = cmode32;
  v.cpcv_embargo_ = emb;
  v.fit_book_size_ = book;
  v.wp_gross_leverage_ = gl;
  v.wp_truncation_ = trunc;
  v.wp_winsorize_limit_ = wl;
  v.commission_per_dollar_bps_ = pdb;
  v.pool_pnl_ =
      (pool_count == 0) ? std::span<const atx::f64>{} : f64_alias(b, pool_off, pool_count);
  v.panel_blob_ = panel_blob;
  v.genome_offsets_ = std::move(offsets);
  v.payload_ = b;
  return v;
}

factory::FitnessCfg MineInputView::fitness_cfg() const noexcept {
  factory::FitnessCfg cfg;
  cfg.trial_count = trial_count_;
  cfg.cpcv.n_groups = cpcv_n_groups_;
  cfg.cpcv.n_test_groups = cpcv_n_test_groups_;
  cfg.cpcv.embargo = cpcv_embargo_;
  cfg.book_size = fit_book_size_;
  return cfg;
}

atx::engine::WeightPolicy MineInputView::weight_policy() const noexcept {
  atx::engine::WeightPolicy p;
  // Reconstruct the FULL Transform taxonomy from the serialized numeric value (the
  // serializer writes static_cast<u32>(policy.transform)). The mapping is the closed
  // enum's frozen wire contract — Rank=0, ZScore=1, Raw=2 (weight_policy.hpp). An
  // exhaustive switch with NO `default` matches the engine's closed-taxonomy discipline:
  // a future enumerator becomes a /W4 /WX compile error HERE (forcing the wire mapping to
  // be extended) rather than silently collapsing to Rank. A serialized value outside the
  // closed taxonomy is an impossible input on the trusted serialize/deserialize seam; the
  // initial Rank default below covers it without widening the switch.
  switch (static_cast<atx::engine::Transform>(wp_transform_)) {
  case atx::engine::Transform::Rank:
    p.transform = atx::engine::Transform::Rank;
    break;
  case atx::engine::Transform::ZScore:
    p.transform = atx::engine::Transform::ZScore;
    break;
  case atx::engine::Transform::Raw:
    p.transform = atx::engine::Transform::Raw;
    break;
  }
  p.industry_neutral = (wp_flags_ & 1U) != 0U;
  p.dollar_neutral = (wp_flags_ & 2U) != 0U;
  p.gross_leverage = wp_gross_leverage_;
  p.truncation = wp_truncation_;
  p.winsorize_limit = wp_winsorize_limit_;
  return p;
}

exec::ExecutionSimulator MineInputView::sim() const {
  // extract_streams / fitness consume ONLY the sim's turnover cost rate, which is keyed
  // off the commission cfg (mode + per_dollar_bps). Rebuild a default sim carrying that
  // exact commission so turnover_cost_rate(sim) matches the parent's bit-for-bit; the
  // other (fill/slippage/impact/latency/volume-cap) cfgs are unused at weight granularity.
  exec::CommissionCfg comm;
  comm.mode = (commission_mode_ == static_cast<atx::u32>(exec::CommissionMode::PerDollar))
                  ? exec::CommissionMode::PerDollar
                  : exec::CommissionMode::PerShare;
  comm.per_dollar_bps = commission_per_dollar_bps_;
  return exec::ExecutionSimulator{exec::FillCfg{},    exec::SlippageCfg{}, exec::ImpactCfg{}, comm,
                                  exec::LatencyCfg{}, exec::VolumeCapCfg{}};
}

Result<alpha::Panel> MineInputView::panel() const {
  // The embedded blob is a panel-only eval payload; reuse the S7.5c parser + Panel
  // rebuild. Its column spans alias the InputView bytes (valid for this view's lifetime).
  ATX_TRY(const EvalInputView ev, EvalInputView::parse(InputView{panel_blob_}));
  return ev.panel();
}

Result<factory::Genome> MineInputView::genome(atx::usize k, const alpha::Library &lib) const {
  if (k >= n_genomes_) {
    return Err(ErrorCode::OutOfRange, "MineInputView::genome: index out of range");
  }
  const atx::usize begin = static_cast<atx::usize>(genome_offsets_[k]);
  const atx::usize end = static_cast<atx::usize>(genome_offsets_[k + 1]);
  // parse() proved begin <= end <= payload size; SliceReader enforces the bound.
  SliceReader sr{payload_, begin, end};

  atx::u64 canon_hash = 0;
  if (!sr.read_pod(canon_hash)) {
    return Err(ErrorCode::InvalidArgument, "MineInputView::genome: truncated canon_hash");
  }

  // String pool.
  atx::u32 n_strings = 0;
  if (!sr.read_pod(n_strings)) {
    return Err(ErrorCode::InvalidArgument, "MineInputView::genome: truncated string count");
  }
  std::vector<std::string> strings;
  strings.reserve(n_strings);
  for (atx::u32 i = 0; i < n_strings; ++i) {
    std::string s;
    if (!sr.read_string(s)) {
      return Err(ErrorCode::InvalidArgument, "MineInputView::genome: truncated string pool");
    }
    strings.push_back(std::move(s));
  }

  // Node records.
  atx::u32 n_nodes = 0;
  if (!sr.read_pod(n_nodes)) {
    return Err(ErrorCode::InvalidArgument, "MineInputView::genome: truncated node count");
  }
  std::vector<alpha::Expr> wire_nodes;
  wire_nodes.reserve(n_nodes);
  std::vector<atx::u32> op_indices; // per node: op-name dict index, 0xFFFFFFFF if none
  op_indices.reserve(n_nodes);
  for (atx::u32 i = 0; i < n_nodes; ++i) {
    atx::u8 kind = 0, dollar = 0, opcode = 0, n_hparams = 0;
    atx::u32 name_id = 0, a = 0, b = 0, c = 0, op_idx = 0;
    atx::f64 value = 0.0, h0 = 0.0, h1 = 0.0;
    if (!sr.read_pod(kind) || !sr.read_pod(dollar) || !sr.read_pod(opcode) ||
        !sr.read_pod(n_hparams) || !sr.read_pod(name_id) || !sr.read_pod(a) || !sr.read_pod(b) ||
        !sr.read_pod(c) || !sr.read_pod(op_idx) || !sr.read_pod(value) || !sr.read_pod(h0) ||
        !sr.read_pod(h1)) {
      return Err(ErrorCode::InvalidArgument, "MineInputView::genome: truncated node record");
    }
    if (kind > static_cast<atx::u8>(alpha::Expr::Kind::Member)) {
      return Err(ErrorCode::InvalidArgument, "MineInputView::genome: bad node kind");
    }
    alpha::Expr e;
    e.kind = static_cast<alpha::Expr::Kind>(kind);
    e.dollar = (dollar != 0U);
    e.opcode = static_cast<alpha::OpCode>(opcode);
    e.value = value;
    e.name_id = name_id;
    e.a = a;
    e.b = b;
    e.c = c;
    e.hparams[0] = h0;
    e.hparams[1] = h1;
    e.n_hparams = n_hparams;
    e.op = nullptr; // remapped after the op-name dictionary is read
    wire_nodes.push_back(e);
    op_indices.push_back(op_idx);
  }

  atx::u32 root = 0;
  if (!sr.read_pod(root)) {
    return Err(ErrorCode::InvalidArgument, "MineInputView::genome: truncated root");
  }

  // Op-name dictionary; remap each name -> lib.find (Err on an unknown op = untrusted).
  atx::u32 n_op_names = 0;
  if (!sr.read_pod(n_op_names)) {
    return Err(ErrorCode::InvalidArgument, "MineInputView::genome: truncated op-name count");
  }
  std::vector<const alpha::OpSig *> op_sigs;
  op_sigs.reserve(n_op_names);
  for (atx::u32 i = 0; i < n_op_names; ++i) {
    std::string name;
    if (!sr.read_string(name)) {
      return Err(ErrorCode::InvalidArgument, "MineInputView::genome: truncated op name");
    }
    const alpha::OpSig *sig = lib.find(name);
    if (sig == nullptr) {
      return Err(ErrorCode::InvalidArgument, "MineInputView::genome: unknown op name");
    }
    op_sigs.push_back(sig);
  }

  // Validate node references + remap Call op pointers, then rebuild the Ast arena by
  // replaying nodes through the builder (children precede parents — the arena is
  // topologically ordered, so name_id/child references are all backward and valid).
  alpha::Ast ast;
  for (atx::u32 i = 0; i < n_nodes; ++i) {
    alpha::Expr e = wire_nodes[i];
    // Child references must be < i (topological) or kNoExpr.
    const atx::u32 kids[3] = {e.a, e.b, e.c};
    for (const atx::u32 child : kids) {
      if (child != alpha::kNoExpr && child >= i) {
        return Err(ErrorCode::InvalidArgument, "MineInputView::genome: forward/oob child ref");
      }
    }
    if (e.kind == alpha::Expr::Kind::Field || e.kind == alpha::Expr::Kind::Member) {
      if (e.name_id >= strings.size()) {
        return Err(ErrorCode::InvalidArgument, "MineInputView::genome: name_id out of range");
      }
      // Re-intern the name into the rebuilt Ast's own pool (ids match because we emit the
      // pool in id order and intern in id order).
      const atx::u32 new_id = ast.intern(strings[e.name_id]);
      ATX_ASSERT(new_id == e.name_id); // dense, in-order interning preserves the id
      e.name_id = new_id;
    }
    if (e.kind == alpha::Expr::Kind::Call) {
      const atx::u32 op_idx = op_indices[i];
      if (op_idx >= op_sigs.size()) {
        return Err(ErrorCode::InvalidArgument, "MineInputView::genome: call op index out of range");
      }
      e.op = op_sigs[op_idx];
    }
    const alpha::ExprId added = ast.add(e);
    ATX_ASSERT(added == i); // dense append preserves ExprId == arena index
    (void)added;
  }
  if (root >= n_nodes && n_nodes != 0U) {
    return Err(ErrorCode::InvalidArgument, "MineInputView::genome: root out of range");
  }
  ast.add_root(std::string{}, root);

  // Re-analyze: pure, bit-faithful rebuild of the Analysis (the round-trip proof). A
  // genome whose bytes do not typecheck is rejected (untrusted-input fault).
  ATX_TRY(alpha::Analysis analysis, alpha::analyze(ast));
  return factory::Genome{std::move(ast), std::move(analysis), canon_hash};
}

// ===========================================================================
//  Worker-side pool snapshot worst_corr — byte-identical to LibraryPool::worst_corr.
// ===========================================================================
namespace {

// A PoolView over the serialized run-start admitted-pnl snapshot. Rebuilds the SAME
// SimHash index library::worst_corr_to_pool uses (CorrNeighborIndex(seed, T, K=64), add
// each admitted stream in AlphaId order) and computes MAX |corr| exactly over a
// candidate's recalled neighbors via combine::pairwise_complete_corr — so the score
// equals the parent's LibraryPool path at run start. An empty snapshot -> 0.
class SnapshotPoolView final : public factory::PoolView {
public:
  SnapshotPoolView(std::span<const atx::f64> pool_pnl, atx::usize n_alphas, atx::usize n_periods,
                   atx::u64 seed)
      : pool_pnl_{pool_pnl}, n_alphas_{n_alphas}, n_periods_{n_periods} {
    if (n_alphas_ == 0U || n_periods_ == 0U) {
      return; // empty pool: worst_corr is 0 (no index built)
    }
    index_.emplace(seed, n_periods_, kCorrK);
    for (atx::usize a = 0; a < n_alphas_; ++a) {
      index_->add(combine::AlphaId{static_cast<atx::u32>(a)}, stream(a));
    }
  }

  [[nodiscard]] atx::f64 worst_corr(std::span<const atx::f64> pnl) const override {
    if (!index_.has_value()) {
      return 0.0;
    }
    atx::f64 worst = 0.0;
    for (const combine::AlphaId id : index_->neighbors(pnl)) {
      const atx::f64 c = combine::pairwise_complete_corr(pnl, stream(id.value));
      const atx::f64 ac = (c < 0.0) ? -c : c;
      worst = (ac > worst) ? ac : worst;
    }
    return worst;
  }

private:
  // The kCorrK SimHash hyperplanes must match library::Library::kCorrK (64).
  static constexpr atx::u32 kCorrK = 64U;

  [[nodiscard]] std::span<const atx::f64> stream(atx::usize a) const noexcept {
    return pool_pnl_.subspan(a * n_periods_, n_periods_);
  }

  std::span<const atx::f64> pool_pnl_;
  atx::usize n_alphas_;
  atx::usize n_periods_;
  std::optional<library::CorrNeighborIndex> index_;
};

} // namespace

// ===========================================================================
//  score_one_genome — the shared scoring map body (process + in-process proof).
// ===========================================================================
Result<MineGenomeResult> score_one_genome(const MineInputView &v, atx::usize k,
                                          const alpha::Library &lib) {
  if (k >= v.n_genomes()) {
    return Err(ErrorCode::OutOfRange, "score_one_genome: genome id out of range");
  }
  // Reconstruct the genome (op-name remap + re-analyze). A genome whose bytes fail to
  // rebuild/typecheck is a malformed-input fault -> Err (the parent never produced such
  // a genome; an honest run always round-trips).
  ATX_TRY(const factory::Genome g, v.genome(k, lib));

  ATX_TRY(const alpha::Panel panel, v.panel());
  const atx::engine::WeightPolicy policy = v.weight_policy();
  const exec::ExecutionSimulator sim = v.sim();
  const factory::FitnessCfg fit_cfg = v.fitness_cfg();

  MineGenomeResult out;

  // detail_eval_streams-equivalent: compile + evaluate + extract_streams. A compile/eval
  // failure is the in-process "silently dropped, sorts last" case -> ok=0 (NOT an Err).
  auto prog = alpha::compile(g.ast, g.analysis);
  if (!prog.has_value()) {
    return out; // ok == 0
  }
  alpha::Engine engine{panel};
  auto ss = engine.evaluate(*prog);
  if (!ss.has_value()) {
    return out; // ok == 0
  }
  auto strm = alpha::extract_streams(*ss, policy, panel, sim);
  if (!strm.has_value()) {
    return out; // ok == 0
  }
  if (strm->n_alphas() == 0U) {
    return out; // ok == 0 (the S3-6 0-alpha guard; pnl(0)/positions(0,.) would abort)
  }

  // Pool-aware fitness vs. the RUN-START snapshot (dsr pool-independent; raw's redundancy
  // = MAX |corr| over the snapshot, byte-identical to the parent's run-start LibraryPool).
  const SnapshotPoolView view{v.pool_pnl(), v.pool_n_alphas(), v.n_periods(), v.pool_seed()};
  auto fit = factory::pool_aware_fitness(g, view, panel, policy, sim, fit_cfg);
  if (fit.has_value()) {
    out.dsr = fit->dsr;
    out.raw = fit->raw;
  }
  // (A fitness Err leaves dsr=raw=0 — exactly the in-process rank_by_deflated_fitness
  // fallback "a genome whose fitness errors sorts last".)

  out.ok = 1U;
  out.streams = std::move(*strm);
  return out;
}

// ===========================================================================
//  mine_shard — the process-portable MINE body.
// ===========================================================================
Status mine_shard(InputView in, ShardId k, std::span<std::byte> slot) noexcept {
  Result<MineInputView> parsed = MineInputView::parse(in);
  if (!parsed) {
    return Err(parsed.error());
  }
  const MineInputView &v = *parsed;
  if (k >= v.n_genomes()) {
    return Err(ErrorCode::OutOfRange, "mine_shard: genome id out of range");
  }

  // The worker's run-wide op catalogue: a process-lifetime default Library (the same
  // built-in catalogue the parent's genomes were built against — the in-grammar search
  // uses only built-in ops, so find() resolves every serialized op name). Static so its
  // OpSig rows outlive the rebuilt genome's Expr::op borrows for this call.
  static const alpha::Library worker_lib;

  Result<MineGenomeResult> scored = score_one_genome(v, k, worker_lib);
  if (!scored) {
    return Err(scored.error());
  }
  const MineGenomeResult &res = *scored;

  // Output slot: { ok:u32, pad:u32, dsr:f64, raw:f64, pnl[T]:f64, pos[T*N]:f64 }.
  // The shapes (T = stream periods == panel.dates(), N = stream instruments) come from
  // the REALIZED stream, NOT v.n_periods() (which is the pool-snapshot T — 0 for an empty
  // pool). A non-ok genome writes ok=0 and a zeroed remainder (the parent reads ok and
  // ignores the streams). ALWAYS-ON guards precede every memcpy (a debug-only assert an
  // NDEBUG build elides before a memcpy is Critical).
  atx::usize stream_periods = 0;
  atx::usize n_inst = 0;
  atx::usize pnl_cells = 0;
  atx::usize pos_cells = 0;
  if (res.ok == 1U) {
    stream_periods = res.streams.n_periods();
    n_inst = res.streams.n_instruments();
    pnl_cells = stream_periods;
    if (!checked_mul(stream_periods, n_inst, pos_cells)) {
      return Err(ErrorCode::Internal, "mine_shard: pos cell count overflows");
    }
  }
  // need_bytes = header(24) + pnl_cells*8 + pos_cells*8 (overflow-checked).
  atx::usize pnl_bytes = 0, pos_bytes = 0, need_bytes = kMineSlotHeaderBytes;
  if (!checked_mul(pnl_cells, sizeof(atx::f64), pnl_bytes) ||
      !checked_mul(pos_cells, sizeof(atx::f64), pos_bytes)) {
    return Err(ErrorCode::Internal, "mine_shard: output byte size overflows");
  }
  // Guard the SUM too: each product is checked above, but 24 + pnl + pos can still wrap, which
  // would yield a small need_bytes that passes the slot check and then OOB-writes the pnl memcpy.
  constexpr atx::usize kUsizeMax = static_cast<atx::usize>(-1);
  if (pnl_bytes > kUsizeMax - need_bytes) {
    return Err(ErrorCode::Internal, "mine_shard: output byte size overflows");
  }
  need_bytes += pnl_bytes;
  if (pos_bytes > kUsizeMax - need_bytes) {
    return Err(ErrorCode::Internal, "mine_shard: output byte size overflows");
  }
  need_bytes += pos_bytes;
  // ALWAYS-ON guard: the slot must hold the whole fixed-shape record before any memcpy.
  if (slot.size() < need_bytes) {
    return Err(ErrorCode::InvalidArgument, "mine_shard: slot smaller than needed");
  }

  // Header scalars (memcpy each at its fixed offset; slot.size() >= 24 proved above).
  const atx::u32 ok = res.ok;
  const atx::u32 pad = 0U;
  std::memcpy(slot.data() + 0, &ok, sizeof(atx::u32));
  std::memcpy(slot.data() + 4, &pad, sizeof(atx::u32));
  std::memcpy(slot.data() + 8, &res.dsr, sizeof(atx::f64));
  std::memcpy(slot.data() + 16, &res.raw, sizeof(atx::f64));

  if (res.ok == 1U) {
    // pnl[T] (alpha 0's stream) then pos[T*N] (alpha 0, period-major then inst-minor).
    // SAFETY: need_bytes (validated <= slot.size()) bounds both writes; pnl/pos cells
    // were overflow-checked; res.streams has n_alphas >= 1 (guarded in score_one_genome).
    const std::span<const atx::f64> pnl0 = res.streams.pnl(0);
    // ALWAYS-ON (not ATX_ASSERT): the memcpy below READS pnl_bytes == pnl_cells*8 from
    // pnl0.data(); a debug-only assert would be elided under NDEBUG and a short stream would
    // then OOB-read the source. (pnl_cells derives from the untrusted-bytes-reconstructed panel.)
    if (pnl0.size() != pnl_cells) {
      return Err(ErrorCode::Internal, "mine_shard: pnl stream size != expected cells");
    }
    if (pnl_bytes != 0) {
      std::memcpy(slot.data() + kMineSlotHeaderBytes, pnl0.data(), pnl_bytes);
    }
    // Positions: concatenate each period's cross-section (n_inst f64) in order.
    atx::usize off = kMineSlotHeaderBytes + pnl_bytes;
    for (atx::usize t = 0; t < stream_periods; ++t) {
      const std::span<const atx::f64> cs = res.streams.positions(0, t);
      // ALWAYS-ON source-read guard (same reason as the pnl write above).
      if (cs.size() != n_inst) {
        return Err(ErrorCode::Internal, "mine_shard: position cross-section size != n_inst");
      }
      const atx::usize cs_bytes = cs.size() * sizeof(atx::f64); // == n_inst*8 <= pos_bytes (checked)
      // ALWAYS-ON guard before each cross-section memcpy (NDEBUG-safe bound).
      if (cs_bytes != 0) {
        if (off + cs_bytes > slot.size()) {
          return Err(ErrorCode::Internal, "mine_shard: position write overruns slot");
        }
        std::memcpy(slot.data() + off, cs.data(), cs_bytes);
      }
      off += cs_bytes;
    }
  }
  return atx::core::Ok();
}

} // namespace atx::engine::parallel
