#include "atx/engine/data/panel_store.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <utility>

#include "atx/core/sha256.hpp"
#include "atx/tsdb/mapping.hpp"

namespace atx::engine::data {
namespace {
using namespace atx;
using core::Err; using core::ErrorCode; using core::Ok; using core::Result; using core::Status;
namespace fs = std::filesystem;
constexpr u64 kManifestLimit = 8ULL * 1024 * 1024;
constexpr u64 kChunkLimit = 64ULL * 1024 * 1024;
constexpr u64 kMaxArtifact = 1ULL << 40;
constexpr u64 kMissing64 = 0x7ff8000000000000ULL;
constexpr u32 kMissing32 = 0x7fc00000U;
constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();
static_assert(sizeof(f32) == 4 && sizeof(f64) == 8 && std::numeric_limits<f64>::is_iec559);

bool hash_valid(std::string_view s) {
  return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool text_valid(std::string_view s, usize limit) {
  return !s.empty() && s.size() <= limit && s.find('\0') == std::string_view::npos;
}
u64 align8(u64 n) { return (n + 7U) & ~u64{7}; }
void put(u8* p, u64 value, usize n = 8) {
  for (usize i = 0; i < n; ++i) p[i] = static_cast<u8>(value >> (8 * i));
}
u64 get(const u8* p, usize n = 8) {
  u64 value = 0; for (usize i = 0; i < n; ++i) value |= u64{p[i]} << (8 * i); return value;
}
void number(std::string& out, u64 value) {
  std::array<u8, 8> bytes{}; put(bytes.data(), value);
  out.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
void text(std::string& out, std::string_view value) { number(out, value.size()); out.append(value); }
struct Cursor {
  std::string_view bytes; usize pos{};
  u64 number() {
    if (bytes.size() - pos < 8) throw std::invalid_argument("short integer");
    const auto n = get(reinterpret_cast<const u8*>(bytes.data()) + pos); pos += 8; return n;
  }
  usize count(usize maximum) {
    const auto n = number(); if (n > maximum || n > (bytes.size() - pos) / 8)
      throw std::invalid_argument("oversized count");
    return static_cast<usize>(n);
  }
  std::string text(usize maximum) {
    const auto n = number(); if (n > maximum || n > bytes.size() - pos)
      throw std::invalid_argument("oversized string");
    std::string out(bytes.substr(pos, static_cast<usize>(n))); pos += static_cast<usize>(n); return out;
  }
};
struct Layout {
  std::vector<u64> fields;
  u64 close{}, present{}, tradable{}, clocks{}, bytes{};
};
Layout layout(const PanelStoreConfig& c, usize dates) {
  const u64 cells = u64{dates} * c.instrument_ids.size();
  Layout l; l.fields.reserve(c.fields.size()); l.bytes = 32;
  for (const auto& f : c.fields) {
    l.fields.push_back(l.bytes);
    l.bytes += align8(cells * (f.precision == PanelStorePrecision::Float32V2 ? 4 : 8));
  }
  l.close = l.bytes; l.bytes += cells * 8;
  l.present = l.bytes; l.bytes += align8(cells);
  l.tradable = l.bytes; l.bytes += align8(cells);
  l.clocks = l.bytes; l.bytes += u64{dates} * 8;
  return l;
}
std::string chunk_name(usize index) { return "chunk-" + std::to_string(index) + ".bin"; }
struct ChunkInfo { usize begin{}, dates{}; u64 bytes{}; std::string sha; };
struct MappingBudget {
  std::mutex mutex; u64 bytes{}, limit{}; u32 handles{}, handle_limit{};
};

Status write_exclusive(const fs::path& path, std::span<const u8> bytes) {
#if defined(_WIN32)
  std::FILE* file = nullptr;
  if (_wfopen_s(&file, path.c_str(), L"wbx") != 0) file = nullptr;
#else
  auto* file = std::fopen(path.c_str(), "wbx");
#endif
  if (!file) return Err(ErrorCode::IoError, "panel store: exclusive file creation failed");
  const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
  const bool flushed = std::fflush(file) == 0;
  const bool closed = std::fclose(file) == 0;
  if (!written || !flushed || !closed) return Err(ErrorCode::IoError, "panel store: write/flush/close failed");
  return Ok();
}
std::span<const std::byte> byte_span(std::span<const u8> b) { return std::as_bytes(b); }
void encode_config(std::string& out, const PanelStoreConfig& c) {
  number(out, c.session_keys.size()); for (auto v : c.session_keys) number(out, static_cast<u64>(v));
  number(out, c.instrument_ids.size()); for (auto v : c.instrument_ids) number(out, static_cast<u64>(v));
  for (auto v : c.original_indices) number(out, v);
  number(out, c.fields.size()); for (const auto& f : c.fields) {
    text(out, f.name); number(out, static_cast<u8>(f.basis)); number(out, static_cast<u8>(f.precision));
  }
  text(out, c.instrument_namespace); text(out, c.recipe); text(out, c.membership_sha256);
  number(out, c.parents.size()); for (const auto& p : c.parents) { text(out, p.role); text(out, p.sha256); }
  number(out, c.chunk_dates); number(out, static_cast<u64>(c.sealed_end_exclusive));
  number(out, c.max_working_bytes); number(out, c.max_mapped_bytes); number(out, c.max_open_chunks);
}
PanelStoreConfig decode_config(Cursor& r) {
  PanelStoreConfig c;
  const auto d = r.count(100000); c.session_keys.reserve(d);
  for (usize i = 0; i < d; ++i) c.session_keys.push_back(std::bit_cast<i64>(r.number()));
  const auto n = r.count(100000); c.instrument_ids.reserve(n); c.original_indices.reserve(n);
  for (usize i = 0; i < n; ++i) c.instrument_ids.push_back(std::bit_cast<i64>(r.number()));
  for (usize i = 0; i < n; ++i) c.original_indices.push_back(r.number());
  const auto f = r.count(512); c.fields.reserve(f);
  for (usize i = 0; i < f; ++i) {
    auto name = r.text(128); const auto basis = r.number(), precision = r.number();
    if (basis < 1 || basis > 3 || precision < 1 || precision > 2) throw std::invalid_argument("field recipe");
    c.fields.push_back({std::move(name), static_cast<LevelBasis>(basis), static_cast<PanelStorePrecision>(precision)});
  }
  c.instrument_namespace = r.text(128); c.recipe = r.text(1024 * 1024); c.membership_sha256 = r.text(64);
  const auto p = r.count(4096); c.parents.reserve(p);
  for (usize i = 0; i < p; ++i) { auto role = r.text(128), sha = r.text(64); c.parents.push_back({std::move(role), std::move(sha)}); }
  const auto chunk_dates = r.number(); if (chunk_dates > 256) throw std::invalid_argument("chunk dates");
  c.chunk_dates = static_cast<usize>(chunk_dates); c.sealed_end_exclusive = std::bit_cast<i64>(r.number());
  c.max_working_bytes = r.number(); c.max_mapped_bytes = r.number(); const auto handles = r.number();
  if (handles > 256) throw std::invalid_argument("handle limit"); c.max_open_chunks = static_cast<u32>(handles);
  return c;
}
bool canonical_numeric(const u8* p, usize width, bool positive = false) {
  const auto bits = get(p, width);
  const auto v = width == 4 ? static_cast<f64>(std::bit_cast<f32>(static_cast<u32>(bits))) : std::bit_cast<f64>(bits);
  if (std::isnan(v)) return bits == (width == 4 ? kMissing32 : kMissing64);
  return std::isfinite(v) && (!positive || v > 0);
}
f64 value(const u8* p, usize width) {
  return width == 4 ? static_cast<f64>(std::bit_cast<f32>(static_cast<u32>(get(p, 4)))) : std::bit_cast<f64>(get(p));
}
} // namespace

Result<PanelStoreSizing> preflight_panel_store(const PanelStoreConfig& c) {
  if (c.session_keys.empty() || c.session_keys.size() > 100000 || c.instrument_ids.empty() ||
      c.instrument_ids.size() > 100000 || c.original_indices.size() != c.instrument_ids.size() ||
      c.fields.empty() || c.fields.size() > 512 || c.parents.empty() || c.parents.size() > 4096 ||
      !c.chunk_dates || c.chunk_dates > 256 || c.sealed_end_exclusive <= 0 ||
      c.sealed_end_exclusive > 1'577'836'800'000'000'000LL || !c.max_open_chunks || c.max_open_chunks > 256 ||
      !text_valid(c.instrument_namespace, 128) || !text_valid(c.recipe, 1024 * 1024) ||
      !hash_valid(c.membership_sha256))
    return Err(ErrorCode::InvalidArgument, "panel store: invalid shape/recipe/seal");
  for (usize t = 0; t < c.session_keys.size(); ++t)
    if (c.session_keys[t] <= 0 || c.session_keys[t] >= c.sealed_end_exclusive ||
        (t && c.session_keys[t - 1] >= c.session_keys[t]))
      return Err(ErrorCode::InvalidArgument, "panel store: sessions must be positive increasing and sealed");
  for (usize i = 0; i < c.instrument_ids.size(); ++i)
    if (c.instrument_ids[i] <= 0 || (i && c.instrument_ids[i - 1] >= c.instrument_ids[i]))
      return Err(ErrorCode::InvalidArgument, "panel store: IDs must be canonical positive sorted unique integers");
  const usize full = c.session_keys.size() / c.chunk_dates, rem = c.session_keys.size() % c.chunk_dates;
  const u64 chunks = full + (rem ? 1U : 0U);
  const u64 metadata = 65536 + u64{c.session_keys.size()} * 16 + u64{c.instrument_ids.size()} * 32 +
      u64{c.fields.size()} * 256 + u64{c.parents.size()} * 256 + u64{c.recipe.size()} * 4 + chunks * 192;
  if (metadata > kManifestLimit || metadata * 4 + 65536 > c.max_working_bytes)
    return Err(ErrorCode::InvalidArgument, "panel store: metadata/working budget exceeded");
  auto indices = c.original_indices; std::sort(indices.begin(), indices.end());
  if (std::adjacent_find(indices.begin(), indices.end()) != indices.end())
    return Err(ErrorCode::InvalidArgument, "panel store: repeated source axis index");
  for (usize f = 0; f < c.fields.size(); ++f) {
    const auto& field = c.fields[f]; const auto basis = static_cast<u8>(field.basis);
    if (!text_valid(field.name, 128) || basis < 1 || basis > 3 ||
        (field.precision != PanelStorePrecision::Float32V2 && field.precision != PanelStorePrecision::ExactFloat64V2))
      return Err(ErrorCode::InvalidArgument, "panel store: invalid field basis/precision");
    for (usize j = 0; j < f; ++j) if (c.fields[j].name == field.name)
      return Err(ErrorCode::InvalidArgument, "panel store: duplicate field");
    if (field.name == "returns" && field.precision != PanelStorePrecision::ExactFloat64V2)
      return Err(ErrorCode::InvalidArgument, "panel store: returns require original-f64 storage");
    if (field.name == "close" && basis != 2)
      return Err(ErrorCode::InvalidArgument, "panel store: close must have adjusted-level basis");
  }
  for (usize p = 0; p < c.parents.size(); ++p) {
    if (!text_valid(c.parents[p].role, 128) || !hash_valid(c.parents[p].sha256))
      return Err(ErrorCode::InvalidArgument, "panel store: invalid parent identity");
    for (usize j = 0; j < p; ++j) if (c.parents[p].role == c.parents[j].role)
      return Err(ErrorCode::InvalidArgument, "panel store: duplicate parent role");
  }
  const auto l = layout(c, std::min(c.chunk_dates, c.session_keys.size()));
  const u64 working = l.bytes + metadata * 4 + 65536;
  const u64 payload = u64{full} * layout(c, c.chunk_dates).bytes + (rem ? layout(c, rem).bytes : 0);
  if (metadata > kManifestLimit || l.bytes > kChunkLimit || working > c.max_working_bytes ||
      l.bytes > c.max_mapped_bytes || payload > kMaxArtifact)
    return Err(ErrorCode::InvalidArgument, "panel store: metadata/chunk/working/mapping/artifact budget exceeded");
  return Ok(PanelStoreSizing{payload, l.bytes, working, metadata});
}

struct PanelStoreWriter::Impl {
  fs::path directory; PanelStoreConfig config; std::vector<ChunkInfo> chunks;
  std::vector<u8> buffer; Layout current; usize next{}, begin{}, count{}; bool failed{}, finished{};
  void start() {
    begin = next; count = std::min(config.chunk_dates, config.session_keys.size() - begin);
    current = layout(config, count); buffer.assign(static_cast<usize>(current.bytes), 0);
    std::memcpy(buffer.data(), "ATPCHN2\0", 8); put(buffer.data() + 8, begin);
    put(buffer.data() + 16, count); put(buffer.data() + 24, config.instrument_ids.size());
  }
  Status flush() {
    failed = true;
    ATX_TRY(auto sha, core::sha256_hex(byte_span(buffer)));
    ATX_TRY_VOID(write_exclusive(directory / chunk_name(chunks.size()), buffer));
    chunks.push_back({begin, count, current.bytes, std::move(sha)});
    buffer.clear(); failed = false; return Ok();
  }
};
PanelStoreWriter::PanelStoreWriter(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
PanelStoreWriter::PanelStoreWriter(PanelStoreWriter&&) noexcept = default;
PanelStoreWriter& PanelStoreWriter::operator=(PanelStoreWriter&&) noexcept = default;
PanelStoreWriter::~PanelStoreWriter() = default;
Result<PanelStoreWriter> PanelStoreWriter::create(const std::string& directory, const PanelStoreConfig& config) {
  ATX_TRY(auto sizing, preflight_panel_store(config)); (void)sizing;
  std::error_code ec;
  if (!fs::create_directory(directory, ec) || ec)
    return Err(ErrorCode::IoError, "panel store: output directory must be fresh with an existing parent");
  auto p = std::make_unique<Impl>(); p->directory = directory; p->config = config;
  p->chunks.reserve((config.session_keys.size() + config.chunk_dates - 1) / config.chunk_dates);
  p->start(); return Ok(PanelStoreWriter(std::move(p)));
}
Status PanelStoreWriter::append_date(usize date, std::span<const std::span<const f64>> fields,
    std::span<const f64> exact_close, std::span<const u8> present,
    std::span<const u8> tradable, i64 decision) {
  if (!impl_ || impl_->failed || impl_->finished || date != impl_->next || date >= impl_->config.session_keys.size())
    return Err(ErrorCode::InvalidArgument, "panel store: invalid append state/order");
  auto& p = *impl_; const auto& c = p.config; const auto n = c.instrument_ids.size();
  if (fields.size() != c.fields.size() || exact_close.size() != n || present.size() != n || tradable.size() != n ||
      decision < 0 || decision >= c.session_keys[date])
    return Err(ErrorCode::InvalidArgument, "panel store: row shape or strict membership clock");
  for (usize i = 0; i < n; ++i) {
    if (present[i] > 1 || tradable[i] > 1 || (tradable[i] && decision == 0))
      return Err(ErrorCode::InvalidArgument, "panel store: invalid independent masks/clock");
    if (present[i] && !std::isnan(exact_close[i]) && (!std::isfinite(exact_close[i]) || exact_close[i] <= 0))
      return Err(ErrorCode::InvalidArgument, "panel store: invalid original-f64 close");
  }
  for (usize f = 0; f < fields.size(); ++f) {
    if (fields[f].size() != n) return Err(ErrorCode::InvalidArgument, "panel store: ragged field row");
    for (usize i = 0; i < n; ++i) if (present[i] && !std::isnan(fields[f][i])) {
      const auto v = fields[f][i];
      if (!std::isfinite(v)) return Err(ErrorCode::InvalidArgument, "panel store: infinite field");
      if (c.fields[f].precision == PanelStorePrecision::Float32V2 &&
          (std::abs(v) > static_cast<f64>((std::numeric_limits<f32>::max)()) ||
           (v != 0 && static_cast<f32>(v) == 0)))
        return Err(ErrorCode::InvalidArgument, "panel store: f32 overflow/underflow");
    }
  }
  if (p.buffer.empty()) p.start();
  const auto local = date - p.begin; const auto cell = local * n;
  for (usize f = 0; f < fields.size(); ++f) {
    const usize width = c.fields[f].precision == PanelStorePrecision::Float32V2 ? 4 : 8;
    for (usize i = 0; i < n; ++i) {
      const auto v = fields[f][i]; const bool missing = !present[i] || std::isnan(v);
      const u64 bits = width == 4 ? (missing ? kMissing32 : std::bit_cast<u32>(static_cast<f32>(v))) :
          (missing ? kMissing64 : std::bit_cast<u64>(v));
      put(p.buffer.data() + p.current.fields[f] + (cell + i) * width, bits, width);
    }
  }
  for (usize i = 0; i < n; ++i) put(p.buffer.data() + p.current.close + (cell + i) * 8,
      !present[i] || std::isnan(exact_close[i]) ? kMissing64 : std::bit_cast<u64>(exact_close[i]));
  std::copy(present.begin(), present.end(), p.buffer.begin() + static_cast<std::ptrdiff_t>(p.current.present + cell));
  std::copy(tradable.begin(), tradable.end(), p.buffer.begin() + static_cast<std::ptrdiff_t>(p.current.tradable + cell));
  put(p.buffer.data() + p.current.clocks + local * 8, static_cast<u64>(decision));
  ++p.next;
  if (p.next == p.begin + p.count) return p.flush();
  return Ok();
}
Result<std::string> PanelStoreWriter::finish() {
  if (!impl_ || impl_->failed || impl_->finished || impl_->next != impl_->config.session_keys.size())
    return Err(ErrorCode::InvalidArgument, "panel store: incomplete/failed/already-published writer");
  auto& p = *impl_; p.failed = true;
  std::string body("ATPSTR2\0", 8); number(body, 2); encode_config(body, p.config);
  number(body, p.chunks.size()); for (const auto& c : p.chunks) {
    number(body, c.begin); number(body, c.dates); number(body, c.bytes); text(body, c.sha);
  }
  ATX_TRY(auto integrity, core::sha256_hex(body)); body += integrity;
  if (body.size() > kManifestLimit) return Err(ErrorCode::InvalidArgument, "panel store: oversized manifest");
  ATX_TRY(auto hash, core::sha256_hex(body));
  const auto bytes = std::span{reinterpret_cast<const u8*>(body.data()), body.size()};
  ATX_TRY_VOID(write_exclusive(p.directory / "manifest.partial", bytes));
  std::error_code ec; fs::create_hard_link(p.directory / "manifest.partial", p.directory / "manifest.bin", ec);
  if (ec) return Err(ErrorCode::IoError, "panel store: no-replace manifest publication failed");
  fs::remove(p.directory / "manifest.partial", ec);
  if (ec) return Err(ErrorCode::IoError, "panel store: manifest partial cleanup failed");
  p.finished = true; p.failed = false; return Ok(std::move(hash));
}

struct PanelStore::Impl {
  fs::path directory; PanelStoreConfig config; std::vector<ChunkInfo> chunks; std::string hash;
  std::shared_ptr<MappingBudget> budget;
};
struct PanelStoreChunk::Impl {
  tsdb::Mapping mapping; Layout offsets; usize begin{}, dates{}, instruments{};
  std::vector<PanelStorePrecision> precisions;
  std::shared_ptr<MappingBudget> budget; u64 reserved{};
  ~Impl() {
    mapping = tsdb::Mapping{}; // release OS resources before permitting another reservation
    if (reserved) { std::lock_guard lock(budget->mutex); budget->bytes -= reserved; --budget->handles; }
  }
};
PanelStore::PanelStore(std::shared_ptr<Impl> p) : impl_(std::move(p)) {}
PanelStoreChunk::PanelStoreChunk(std::shared_ptr<const Impl> p) : impl_(std::move(p)) {}
const PanelStoreConfig& PanelStore::config() const noexcept { return impl_->config; }
std::string_view PanelStore::manifest_sha256() const noexcept { return impl_->hash; }
usize PanelStore::chunks() const noexcept { return impl_->chunks.size(); }
Result<PanelStore> PanelStore::open(const std::string& directory, std::string_view expected,
                                  u64 mapped_limit, u32 handle_limit) {
  if ((!expected.empty() && !hash_valid(expected)) || !mapped_limit || !handle_limit || handle_limit > 256)
    return Err(ErrorCode::InvalidArgument, "panel store: invalid open budget/hash");
  const auto manifest = fs::path(directory) / "manifest.bin";
  std::ifstream file(manifest, std::ios::binary | std::ios::ate);
  if (!file) return Err(ErrorCode::IoError, "panel store: complete manifest missing");
  const auto size = file.tellg();
  if (size < 80 || size > static_cast<std::streamoff>(kManifestLimit))
    return Err(ErrorCode::ParseError, "panel store: invalid manifest size");
  std::string bytes(static_cast<usize>(size), '\0'); file.seekg(0); file.read(bytes.data(), static_cast<std::streamsize>(size));
  if (!file) return Err(ErrorCode::IoError, "panel store: manifest read failed");
  ATX_TRY(auto hash, core::sha256_hex(bytes));
  if (!expected.empty() && expected != hash) return Err(ErrorCode::InvalidArgument, "panel store: external manifest hash mismatch");
  const auto body = std::string_view(bytes).substr(0, bytes.size() - 64);
  ATX_TRY(auto integrity, core::sha256_hex(body));
  if (std::string_view(bytes).substr(body.size()) != integrity || body.substr(0, 8) != std::string_view("ATPSTR2\0", 8))
    return Err(ErrorCode::ParseError, "panel store: bad manifest integrity/version");
  try {
    Cursor r{body, 8}; if (r.number() != 2) throw std::invalid_argument("unsupported version");
    auto p = std::make_shared<Impl>(); p->directory = directory; p->config = decode_config(r); p->hash = std::move(hash);
    ATX_TRY(auto sizing, preflight_panel_store(p->config)); (void)sizing;
    p->budget = std::make_shared<MappingBudget>();
    p->budget->limit = std::min(p->config.max_mapped_bytes, mapped_limit);
    p->budget->handle_limit = std::min(p->config.max_open_chunks, handle_limit);
    const auto n = r.count(100000); const auto& c = p->config;
    if (n != (c.session_keys.size() + c.chunk_dates - 1) / c.chunk_dates)
      throw std::invalid_argument("chunk count");
    p->chunks.reserve(n); usize begin = 0;
    for (usize i = 0; i < n; ++i) {
      const auto b = r.number(), d = r.number(), extent = r.number(); auto sha = r.text(64);
      const auto want = std::min(c.chunk_dates, c.session_keys.size() - begin);
      if (b != begin || d != want || extent != layout(c, want).bytes || !hash_valid(sha))
        throw std::invalid_argument("chunk geometry/hash");
      std::error_code ec; const auto actual = fs::file_size(p->directory / chunk_name(i), ec);
      if (ec || actual != extent) return Err(ErrorCode::IoError, "panel store: chunk absent/extent mismatch");
      p->chunks.push_back({begin, want, extent, std::move(sha)}); begin += want;
    }
    if (r.pos != body.size()) throw std::invalid_argument("trailing manifest bytes");
    return Ok(PanelStore(std::move(p)));
  } catch (const std::exception& e) {
    return Err(ErrorCode::ParseError, std::string("panel store: manifest ") + e.what());
  }
}
Result<PanelStoreChunk> PanelStore::open_chunk(usize index) const {
  if (index >= impl_->chunks.size()) return Err(ErrorCode::InvalidArgument, "panel store: chunk index out of range");
  const auto& c = impl_->config; const auto& info = impl_->chunks[index];
  auto chunk = std::make_shared<PanelStoreChunk::Impl>();
  chunk->budget = impl_->budget;
  {
    std::lock_guard lock(chunk->budget->mutex);
    if (info.bytes > chunk->budget->limit - chunk->budget->bytes || chunk->budget->handles >= chunk->budget->handle_limit)
      return Err(ErrorCode::InvalidArgument, "panel store: live mapping/handle budget exceeded");
    chunk->budget->bytes += info.bytes; ++chunk->budget->handles; chunk->reserved = info.bytes;
  }
  ATX_TRY(chunk->mapping, tsdb::Mapping::map_file_ro(
      (impl_->directory / chunk_name(index)).string(), info.bytes, info.bytes));
  if (chunk->mapping.size() != info.bytes) return Err(ErrorCode::IoError, "panel store: captured chunk size changed");
  const auto* p = chunk->mapping.base();
  ATX_TRY(auto hash, core::sha256_hex(byte_span(std::span{p, chunk->mapping.size()})));
  if (hash != info.sha || std::memcmp(p, "ATPCHN2\0", 8) || get(p + 8) != info.begin ||
      get(p + 16) != info.dates || get(p + 24) != c.instrument_ids.size())
    return Err(ErrorCode::ParseError, "panel store: captured chunk integrity/header mismatch");
  chunk->offsets = layout(c, info.dates); chunk->begin = info.begin; chunk->dates = info.dates;
  chunk->instruments = c.instrument_ids.size(); const auto& l = chunk->offsets;
  const auto cells = info.dates * chunk->instruments;
  for (usize f = 0; f < c.fields.size(); ++f) {
    chunk->precisions.push_back(c.fields[f].precision);
    const usize width = c.fields[f].precision == PanelStorePrecision::Float32V2 ? 4 : 8;
    for (usize i = 0; i < cells; ++i)
      if (!canonical_numeric(p + l.fields[f] + i * width, width) ||
          (!p[l.present + i] && get(p + l.fields[f] + i * width, width) != (width == 4 ? kMissing32 : kMissing64)))
        return Err(ErrorCode::ParseError, "panel store: noncanonical numeric/missing field");
  }
  for (usize t = 0; t < info.dates; ++t) {
    const auto clock = std::bit_cast<i64>(get(p + l.clocks + t * 8));
    if (clock < 0 || clock >= c.session_keys[info.begin + t])
      return Err(ErrorCode::ParseError, "panel store: invalid strict membership clock");
    for (usize i = 0; i < chunk->instruments; ++i) {
      const auto cell = t * chunk->instruments + i;
      if (p[l.present + cell] > 1 || p[l.tradable + cell] > 1 ||
          (p[l.tradable + cell] && clock == 0) || !canonical_numeric(p + l.close + cell * 8, 8, true) ||
          (!p[l.present + cell] && get(p + l.close + cell * 8) != kMissing64))
        return Err(ErrorCode::ParseError, "panel store: invalid masks/exact close/clock");
    }
  }
  return Ok(PanelStoreChunk(std::move(chunk)));
}
usize PanelStoreChunk::begin_date() const noexcept { return impl_->begin; }
usize PanelStoreChunk::dates() const noexcept { return impl_->dates; }
usize PanelStoreChunk::instruments() const noexcept { return impl_->instruments; }
Status PanelStoreChunk::read_field_row(usize field, usize date, std::span<f64> output) const {
  if (field >= impl_->precisions.size() || date >= dates() || output.size() != instruments())
    return Err(ErrorCode::InvalidArgument, "panel store: field row shape/index");
  const usize width = impl_->precisions[field] == PanelStorePrecision::Float32V2 ? 4 : 8;
  const auto* p = impl_->mapping.base() + impl_->offsets.fields[field] + date * instruments() * width;
  for (usize i = 0; i < output.size(); ++i) output[i] = value(p + i * width, width);
  return Ok();
}
Status PanelStoreChunk::read_exact_close_row(usize date, std::span<f64> output) const {
  if (date >= dates() || output.size() != instruments())
    return Err(ErrorCode::InvalidArgument, "panel store: exact-close row shape/index");
  const auto* p = impl_->mapping.base() + impl_->offsets.close + date * instruments() * 8;
  for (usize i = 0; i < output.size(); ++i) output[i] = value(p + i * 8, 8);
  return Ok();
}
Result<std::span<const u8>> PanelStoreChunk::present(usize date) const {
  if (date >= dates()) return Err(ErrorCode::InvalidArgument, "panel store: mask date");
  return Ok(std::span<const u8>{impl_->mapping.base() + impl_->offsets.present + date * instruments(), instruments()});
}
Result<std::span<const u8>> PanelStoreChunk::tradable(usize date) const {
  if (date >= dates()) return Err(ErrorCode::InvalidArgument, "panel store: mask date");
  return Ok(std::span<const u8>{impl_->mapping.base() + impl_->offsets.tradable + date * instruments(), instruments()});
}
Result<i64> PanelStoreChunk::membership_decision_key(usize date) const {
  if (date >= dates()) return Err(ErrorCode::InvalidArgument, "panel store: clock date");
  return Ok(std::bit_cast<i64>(get(impl_->mapping.base() + impl_->offsets.clocks + date * 8)));
}
Status PanelStore::forward_returns(usize entry, usize endpoint, usize maturity, std::span<f64> output) const {
  const auto& c = config();
  if (entry >= endpoint || endpoint >= maturity || maturity > c.session_keys.size() || output.size() != c.instrument_ids.size())
    return Err(ErrorCode::InvalidArgument, "panel store: return horizon/maturity/shape");
  // Only one N-cell scratch row. Mapping the same chunk twice is unnecessary.
  const auto scratch_bytes = u64{output.size()} * sizeof(f64);
  if (scratch_bytes > c.max_working_bytes) return Err(ErrorCode::InvalidArgument, "panel store: return scratch budget");
  std::vector<f64> last(output.size());
  {
    ATX_TRY(auto first, open_chunk(entry / c.chunk_dates));
    ATX_TRY_VOID(first.read_exact_close_row(entry % c.chunk_dates, output));
    if (entry / c.chunk_dates == endpoint / c.chunk_dates)
      ATX_TRY_VOID(first.read_exact_close_row(endpoint % c.chunk_dates, last));
  }
  if (entry / c.chunk_dates != endpoint / c.chunk_dates) {
    ATX_TRY(auto final, open_chunk(endpoint / c.chunk_dates));
    ATX_TRY_VOID(final.read_exact_close_row(endpoint % c.chunk_dates, last));
  }
  for (usize i = 0; i < output.size(); ++i) {
    output[i] = std::isfinite(output[i]) && std::isfinite(last[i]) ? last[i] / output[i] - 1.0 : kNaN;
    if (!std::isfinite(output[i])) output[i] = kNaN;
  }
  return Ok();
}

} // namespace atx::engine::data
