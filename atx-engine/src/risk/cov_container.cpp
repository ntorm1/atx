#include "atx/engine/risk/cov_container.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <system_error>
#include <thread>
#include <utility>

#include "atx/core/sha256.hpp"
#include "atx/tsdb/mapping.hpp"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace atx::engine::risk {
namespace {
using namespace atx;
namespace co = atx::core;
namespace fs = std::filesystem;

constexpr u32 max_sections = 64;     // a sanity bound on the section table
constexpr u32 max_arrays = 64;       // ... and on a block's array table
constexpr usize section_kinds = 5;   // the known kinds 1..5
constexpr u64 index_record = sizeof(CovDateIndexEntry);
constexpr u64 hash_record = 32;
constexpr usize default_workers = 8;

co::Error parse_error(const std::string& what) {
  return co::Error{co::ErrorCode::ParseError, "atx.cov-container: " + what};
}
co::Error refusal(const std::string& what) {
  return co::Error{co::ErrorCode::InvalidArgument, "atx.cov-container: " + what};
}

template <class T>
T read_pod(std::span<const u8> bytes, u64 offset) {
  T out{};
  std::memcpy(&out, bytes.data() + offset, sizeof(T));
  return out;
}
template <class T>
void write_pod(std::vector<u8>& out, u64 offset, const T& value) {
  std::memcpy(out.data() + offset, &value, sizeof(T));
}
template <class T>
std::span<const u8> bytes_of(std::span<const T> values) {
  // SAFETY: an object representation of trivially copyable values, read as bytes.
  return {reinterpret_cast<const u8*>(values.data()), values.size_bytes()};
}
template <class T>
std::span<const u8> pod_bytes(const T& value) {
  // SAFETY: as above, one trivially copyable object.
  return {reinterpret_cast<const u8*>(&value), sizeof(T)};
}
bool all_zero(std::span<const u8> bytes) {
  return std::all_of(bytes.begin(), bytes.end(), [](u8 b) { return b == 0; });
}

co::Result<Sha256Bytes> digest_of(std::span<const std::span<const u8>> parts) {
  co::Sha256 sha;
  for (const auto part : parts) ATX_TRY_VOID(sha.update(std::as_bytes(part)));
  ATX_TRY(const auto raw, sha.finalize());
  Sha256Bytes out{};
  for (usize i = 0; i < out.size(); ++i) out[i] = std::to_integer<u8>(raw[i]);
  return co::Ok(out);
}
co::Result<Sha256Bytes> digest_of(std::span<const u8> bytes) {
  const std::array<std::span<const u8>, 1> parts{bytes};
  return digest_of(parts);
}

std::span<const u8> text_bytes(std::string_view text) {
  // SAFETY: char and u8 share size and alignment; the bytes are only read.
  return {reinterpret_cast<const u8*>(text.data()), text.size()};
}

// ---- block layout -----------------------------------------------------------------------------
struct ArraySpec {
  u32 id{}, dtype{};
  u64 count{};
  std::span<const u8> bytes;
};
u64 element_bytes(u32 dtype) { return cov::dtype_bytes(static_cast<cov::Dtype>(dtype)); }
bool finite_positive(f64 x) { return std::isfinite(x) && x > 0; }
template <class T>
ArraySpec array_of(cov::ArrayId id, cov::Dtype dtype, std::span<const T> values) {
  return {static_cast<u32>(id), static_cast<u32>(dtype), values.size(), bytes_of(values)};
}

co::Status check_block(const CovContainerSpec& spec, const CovBlockInput& b) {
  const u64 k = spec.factors(), rows = b.ids.size(), s = spec.dense;
  const auto bad = [](const char* what) { return co::Err(refusal(std::string("block: ") + what)); };
  if (rows > std::numeric_limits<u32>::max()) return bad("too many rows");
  if (b.role_index.size() != rows || b.group.size() != rows || b.specific.size() != rows)
    return bad("row arrays differ in length");
  const bool f32 = spec.f32_exposures;
  if (f32 ? (b.exposures_f32.size() != rows * s || !b.exposures_f64.empty())
          : (b.exposures_f64.size() != rows * s || !b.exposures_f32.empty()))
    return bad("exposures are not rows x dense in the file's dtype");
  if (b.factor_cov.size() != k * k) return bad("factor covariance is not K x K");
  if ((!b.factor_return.empty() && b.factor_return.size() != k) ||
      (!b.factor_flags.empty() && b.factor_flags.size() != k) ||
      (!b.row_flags.empty() && b.row_flags.size() != rows) ||
      (!b.diag.empty() && b.diag.size() != cov::diag_count) ||
      (!b.eigen_gamma.empty() && b.eigen_gamma.size() != k))
    return bad("an optional array has the wrong length");
  if (b.data_cutoff_ns > b.as_of_session_ns) return bad("data cutoff after as_of");
  if (b.history_first_ns > b.data_cutoff_ns) return bad("history first after the cutoff");
  if (b.priced_rows > rows) return bad("priced rows above rows");
  // Row rule: priced rows first, each part in strictly ascending role index.
  for (u64 r = 0; r < rows; ++r) {
    const u32 g = b.group[r];
    if (g > spec.groups) return bad("group above G");
    const bool priced = g >= 1 && finite_positive(b.specific[r]);
    if (r < b.priced_rows && !priced) return bad("a row below P is not priced");
    if (r >= b.priced_rows && priced) return bad("a priced row after the priced rows");
    if (r > 0 && r != b.priced_rows && b.role_index[r] <= b.role_index[r - 1])
      return bad("role index not ascending within its part");
  }
  return co::Ok();
}
} // namespace

co::Status encode_cov_block(const CovContainerSpec& spec, const CovBlockInput& b,
                            std::vector<u8>& out) {
  ATX_TRY_VOID(check_block(spec, b));
  using cov::ArrayId;
  using cov::Dtype;
  std::vector<ArraySpec> arrays{
      array_of(ArrayId::Ids, Dtype::U64, b.ids),
      array_of(ArrayId::RoleIndex, Dtype::U32, b.role_index),
      array_of(ArrayId::Group, Dtype::U32, b.group),
      spec.f32_exposures ? array_of(ArrayId::Exposures, Dtype::F32, b.exposures_f32)
                         : array_of(ArrayId::Exposures, Dtype::F64, b.exposures_f64),
      array_of(ArrayId::Specific, Dtype::F64, b.specific),
      array_of(ArrayId::FactorCov, Dtype::F64, b.factor_cov)};
  if (!b.factor_return.empty())
    arrays.push_back(array_of(ArrayId::FactorReturn, Dtype::F64, b.factor_return));
  if (!b.factor_flags.empty())
    arrays.push_back(array_of(ArrayId::FactorFlags, Dtype::U8, b.factor_flags));
  if (!b.row_flags.empty()) arrays.push_back(array_of(ArrayId::RowFlags, Dtype::U8, b.row_flags));
  if (!b.diag.empty()) arrays.push_back(array_of(ArrayId::Diag, Dtype::F64, b.diag));
  if (!b.eigen_gamma.empty())
    arrays.push_back(array_of(ArrayId::EigenGamma, Dtype::F64, b.eigen_gamma));
  const u64 table_end = sizeof(CovBlockHeader) + arrays.size() * sizeof(CovArrayEntry);
  std::vector<u64> offsets(arrays.size());
  u64 cursor = cov::align_up(table_end, cov::array_align);
  for (usize a = 0; a < arrays.size(); ++a) {
    offsets[a] = cursor;
    cursor = cov::align_up(cursor + arrays[a].bytes.size(), cov::array_align);
  }
  out.assign(static_cast<usize>(cov::align_up(cursor, cov::page_bytes)), u8{0});
  CovBlockHeader header{};
  header.magic = cov::block_magic;
  header.as_of = b.as_of_session_ns;
  header.cutoff = b.data_cutoff_ns;
  header.rows = static_cast<u32>(b.ids.size());
  header.priced = b.priced_rows;
  header.factors = spec.factors();
  header.groups = spec.groups;
  header.dense = spec.dense;
  header.array_count = static_cast<u32>(arrays.size());
  write_pod(out, 0, header);
  for (usize a = 0; a < arrays.size(); ++a) {
    const CovArrayEntry entry{arrays[a].id, arrays[a].dtype, offsets[a], arrays[a].count};
    write_pod(out, sizeof(CovBlockHeader) + a * sizeof(CovArrayEntry), entry);
    if (!arrays[a].bytes.empty())
      std::memcpy(out.data() + offsets[a], arrays[a].bytes.data(), arrays[a].bytes.size());
  }
  return co::Ok();
}

namespace {
template <class T>
std::span<const T> typed(std::span<const u8> bytes, u64 offset, u64 count) {
  // SAFETY: offset is 64-byte aligned within a buffer whose base parse_cov_block checked to be
  // 8-byte aligned, and [offset, offset + count sizeof(T)) lies inside it (checked); the bytes
  // are the object representation of little-endian T written by encode_cov_block.
  return {reinterpret_cast<const T*>(bytes.data() + offset), static_cast<usize>(count)};
}
} // namespace

co::Result<CovBlockView> parse_cov_block(std::span<const u8> bytes, u32 factors, u32 groups,
                                         u32 dense, bool f32_exposures) {
  if (bytes.size() < sizeof(CovBlockHeader))
    return co::Err(parse_error("block shorter than its header"));
  if (reinterpret_cast<std::uintptr_t>(bytes.data()) % alignof(f64) != 0)
    return co::Err(refusal("block bytes are not 8-byte aligned"));
  const auto h = read_pod<CovBlockHeader>(bytes, 0);
  if (h.magic != cov::block_magic) return co::Err(parse_error("block magic"));
  if (h.factors != factors || h.groups != groups || h.dense != dense ||
      factors != 1U + groups + dense)
    return co::Err(parse_error("block geometry"));
  if (h.array_count > max_arrays || h.priced > h.rows)
    return co::Err(parse_error("block array count or priced rows"));
  const u64 table_end = sizeof(CovBlockHeader) + u64{h.array_count} * sizeof(CovArrayEntry);
  if (table_end > bytes.size()) return co::Err(parse_error("block array table out of bounds"));
  const u64 rows = h.rows, k = factors;
  CovBlockView v;
  v.as_of = h.as_of; v.cutoff = h.cutoff; v.rows = h.rows; v.priced = h.priced;
  v.factors = factors; v.groups = groups; v.dense = dense;
  std::array<bool, 12> seen{};
  u64 previous_end = cov::align_up(table_end, cov::array_align);
  for (u32 a = 0; a < h.array_count; ++a) {
    const auto e = read_pod<CovArrayEntry>(bytes, sizeof(CovBlockHeader) + u64{a} * sizeof(CovArrayEntry));
    if (e.offset_in_block % cov::array_align != 0 || e.offset_in_block < previous_end)
      return co::Err(parse_error("block array misaligned or overlapping"));
    const u64 size = element_bytes(e.dtype);
    if (size == 0 || e.count > (bytes.size() - e.offset_in_block) / size)
      return co::Err(parse_error("block array out of bounds"));
    previous_end = e.offset_in_block + e.count * size;
    const u32 base = e.id & ~cov::array_optional;
    if (base == 0 || base > 11) {
      if ((e.id & cov::array_optional) == 0) return co::Err(parse_error("unknown required array"));
      continue; // an unknown optional array is ignored
    }
    const bool optional = base >= 7;
    if (optional != ((e.id & cov::array_optional) != 0) || seen[base])
      return co::Err(parse_error("array id flags or duplicate"));
    seen[base] = true;
    const auto id = static_cast<cov::ArrayId>(e.id);
    using cov::ArrayId;
    using cov::Dtype;
    const auto need = [&](Dtype d, u64 count) {
      return e.dtype == static_cast<u32>(d) && e.count == count;
    };
    bool ok = false;
    switch (id) {
    case ArrayId::Ids:
      ok = need(Dtype::U64, rows);
      if (ok) v.ids = typed<u64>(bytes, e.offset_in_block, rows);
      break;
    case ArrayId::RoleIndex:
      ok = need(Dtype::U32, rows);
      if (ok) v.role_index = typed<u32>(bytes, e.offset_in_block, rows);
      break;
    case ArrayId::Group:
      ok = need(Dtype::U32, rows);
      if (ok) v.group = typed<u32>(bytes, e.offset_in_block, rows);
      break;
    case ArrayId::Exposures:
      ok = need(f32_exposures ? Dtype::F32 : Dtype::F64, rows * dense);
      if (ok && f32_exposures) v.exposures_f32 = typed<f32>(bytes, e.offset_in_block, rows * dense);
      if (ok && !f32_exposures) v.exposures_f64 = typed<f64>(bytes, e.offset_in_block, rows * dense);
      break;
    case ArrayId::Specific:
      ok = need(Dtype::F64, rows);
      if (ok) v.specific = typed<f64>(bytes, e.offset_in_block, rows);
      break;
    case ArrayId::FactorCov:
      ok = need(Dtype::F64, k * k);
      if (ok) v.factor_cov = typed<f64>(bytes, e.offset_in_block, k * k);
      break;
    case ArrayId::FactorReturn:
      ok = need(Dtype::F64, k);
      if (ok) v.factor_return = typed<f64>(bytes, e.offset_in_block, k);
      break;
    case ArrayId::FactorFlags:
      ok = need(Dtype::U8, k);
      if (ok) v.factor_flags = typed<u8>(bytes, e.offset_in_block, k);
      break;
    case ArrayId::RowFlags:
      ok = need(Dtype::U8, rows);
      if (ok) v.row_flags = typed<u8>(bytes, e.offset_in_block, rows);
      break;
    case ArrayId::Diag:
      ok = need(Dtype::F64, cov::diag_count);
      if (ok) v.diag = typed<f64>(bytes, e.offset_in_block, cov::diag_count);
      break;
    case ArrayId::EigenGamma:
      ok = need(Dtype::F64, k);
      if (ok) v.eigen_gamma = typed<f64>(bytes, e.offset_in_block, k);
      break;
    }
    if (!ok) return co::Err(parse_error("array dtype or count"));
  }
  for (usize id = 1; id <= 6; ++id)
    if (!seen[id]) return co::Err(parse_error("a required array is missing"));
  return co::Ok(v);
}

book::FactorRiskView factor_risk_view(const CovBlockView& v, CovViewScratch& scratch) {
  const usize p = v.priced, s = v.dense, k = v.factors;
  book::FactorRiskView out;
  out.groups = v.groups;
  out.styles = v.dense;
  out.group = v.group.first(p);
  out.specific = v.specific.first(p);
  if (!v.exposures_f64.empty()) {
    out.exposures = v.exposures_f64.first(p * s);
  } else {
    scratch.exposures.resize(p * s);
    for (usize i = 0; i < p * s; ++i) scratch.exposures[i] = static_cast<f64>(v.exposures_f32[i]);
    out.exposures = scratch.exposures;
  }
  const bool complete = std::all_of(v.factor_cov.begin(), v.factor_cov.end(),
                                    [](f64 c) { return std::isfinite(c); });
  if (complete) {
    out.covariance = v.factor_cov;
  } else {
    scratch.covariance.resize(k * k);
    for (usize c = 0; c < k * k; ++c)
      scratch.covariance[c] = std::isfinite(v.factor_cov[c]) ? v.factor_cov[c] : 0.0;
    out.covariance = scratch.covariance;
  }
  return out;
}

// ---- writer ---------------------------------------------------------------------------------------
namespace {
struct FileCloser {
  void operator()(std::FILE* f) const noexcept { std::fclose(f); }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

std::FILE* open_exclusive(const fs::path& path) noexcept {
  std::FILE* file = nullptr;
#if defined(_WIN32)
  if (_wfopen_s(&file, path.c_str(), L"wbx") != 0) file = nullptr;
#else
  file = std::fopen(path.c_str(), "wbx");
#endif
  return file;
}
bool sync_file(std::FILE* file) noexcept {
#if defined(_WIN32)
  return _commit(_fileno(file)) == 0;
#else
  return fsync(fileno(file)) == 0;
#endif
}
bool write_all(std::FILE* file, std::span<const u8> bytes) noexcept {
  return bytes.empty() || std::fwrite(bytes.data(), 1U, bytes.size(), file) == bytes.size();
}
} // namespace

struct CovContainerWriter::Impl {
  fs::path final_path, partial_path;
  CovContainerSpec spec;
  FilePtr file;
  u64 cursor{};
  std::vector<CovDateIndexEntry> index;
  std::vector<Sha256Bytes> hashes;
  u32 max_rows{};
  bool done{}, failed{};
  std::vector<u8> block;
};

CovContainerWriter::CovContainerWriter(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
CovContainerWriter::CovContainerWriter(CovContainerWriter&&) noexcept = default;
CovContainerWriter& CovContainerWriter::operator=(CovContainerWriter&&) noexcept = default;
CovContainerWriter::~CovContainerWriter() = default;
usize CovContainerWriter::blocks() const noexcept { return impl_ ? impl_->index.size() : 0; }

co::Result<CovContainerWriter> CovContainerWriter::create(const std::string& path,
                                                          CovContainerSpec spec) {
  if (spec.groups > 4096 || spec.dense > 4096)
    return co::Err(refusal("geometry: at most 4096 groups and 4096 dense columns"));
  if (spec.factor_names.size() != spec.factors())
    return co::Err(refusal("factor names: K names required"));
  for (const auto& name : spec.factor_names)
    if (name.empty() || name.find('\0') != std::string::npos)
      return co::Err(refusal("factor names: empty or holding a NUL"));
  auto impl = std::make_unique<Impl>();
  impl->final_path = fs::path(path);
  impl->partial_path = fs::path(path + ".partial");
  std::error_code ec;
  if (fs::exists(impl->final_path, ec) || ec)
    return co::Err(co::ErrorCode::AlreadyExists, "atx.cov-container: " + path + " exists");
  impl->file.reset(open_exclusive(impl->partial_path));
  if (!impl->file)
    return co::Err(co::ErrorCode::IoError,
                   "atx.cov-container: cannot create " + impl->partial_path.string());
  const std::vector<u8> reserved(cov::header_bytes, u8{0});
  if (!write_all(impl->file.get(), reserved))
    return co::Err(co::ErrorCode::IoError, "atx.cov-container: header reservation");
  impl->cursor = cov::header_bytes;
  impl->spec = std::move(spec);
  return co::Ok(CovContainerWriter(std::move(impl)));
}

co::Status CovContainerWriter::append(const CovBlockInput& b) {
  if (!impl_ || impl_->done || impl_->failed)
    return co::Err(refusal("append after finalize or a failed write"));
  auto& w = *impl_;
  if (!w.index.empty() && b.as_of_session_ns <= w.index.back().as_of_session_ns)
    return co::Err(refusal("as_of not strictly ascending"));
  if ((b.flags & ~cov::known_date_flags) != 0) return co::Err(refusal("unknown date flag"));
  ATX_TRY_VOID(encode_cov_block(w.spec, b, w.block));
  ATX_TRY(const auto hash, digest_of(w.block));
  if (!write_all(w.file.get(), w.block)) {
    w.failed = true;
    return co::Err(co::ErrorCode::IoError, "atx.cov-container: block write");
  }
  const u64 k = w.spec.factors();
  u32 unforecast = 0;
  for (u64 f = 0; f < k; ++f) unforecast += std::isfinite(b.factor_cov[f * k + f]) ? 0U : 1U;
  const bool complete = std::all_of(b.factor_cov.begin(), b.factor_cov.end(),
                                    [](f64 c) { return std::isfinite(c); });
  CovDateIndexEntry e{};
  e.as_of_session_ns = b.as_of_session_ns;
  e.data_cutoff_ns = b.data_cutoff_ns;
  e.history_first_ns = b.history_first_ns;
  e.block_offset = w.cursor;
  e.block_bytes = w.block.size();
  e.rows = static_cast<u32>(b.ids.size());
  e.priced_rows = b.priced_rows;
  e.role_date_index = b.role_date_index;
  e.flags = (b.flags & ~cov::date_factor_complete) | (complete ? cov::date_factor_complete : 0U);
  e.unforecast_factors = unforecast;
  w.index.push_back(e);
  w.hashes.push_back(hash);
  w.cursor += w.block.size();
  w.max_rows = std::max(w.max_rows, e.rows);
  return co::Ok();
}

co::Result<Sha256Bytes> CovContainerWriter::finalize() {
  if (!impl_ || impl_->done || impl_->failed)
    return co::Err(refusal("finalize after finalize or a failed write"));
  auto& w = *impl_;
  if (w.index.empty()) return co::Err(refusal("finalize without a block"));
  const auto& spec = w.spec;
  std::string names;
  for (const auto& n : spec.factor_names) { names += n; names.push_back('\0'); }
  const u64 table = w.cursor; // 4096-aligned: the header and every block are page multiples
  const u64 table_bytes = section_kinds * sizeof(CovSectionEntry);
  const u64 d = w.index.size();
  struct Placed { cov::SectionKind kind; u32 dtype; u64 count, bytes; std::span<const u8> data; };
  const std::array<Placed, 4> metadata{{
      {cov::SectionKind::MetaJson, static_cast<u32>(cov::Dtype::Utf8), spec.meta_json.size(),
       spec.meta_json.size(), text_bytes(spec.meta_json)},
      {cov::SectionKind::FactorNames, static_cast<u32>(cov::Dtype::Utf8), names.size(),
       names.size(), text_bytes(names)},
      {cov::SectionKind::DateIndex, static_cast<u32>(cov::Dtype::U8), d, d * index_record,
       bytes_of(std::span<const CovDateIndexEntry>(w.index))},
      {cov::SectionKind::BlockHashes, static_cast<u32>(cov::Dtype::U8), d, d * hash_record,
       bytes_of(std::span<const Sha256Bytes>(w.hashes))}}};
  std::array<u64, 4> offsets{};
  u64 cursor = table + table_bytes;
  for (usize s = 0; s < metadata.size(); ++s) {
    offsets[s] = cov::align_up(cursor, cov::array_align);
    cursor = offsets[s] + metadata[s].bytes;
  }
  const u64 trailer_at = cov::align_up(cursor, cov::array_align);
  const u64 file_bytes = trailer_at + sizeof(CovTrailer);
  std::vector<u8> tail(static_cast<usize>(file_bytes - table), u8{0});
  for (usize s = 0; s < metadata.size(); ++s) {
    CovSectionEntry e{};
    e.kind = static_cast<u32>(metadata[s].kind);
    e.flags = cov::section_required | cov::section_in_root;
    e.offset = offsets[s];
    e.bytes = metadata[s].bytes;
    e.count = metadata[s].count;
    e.dtype = metadata[s].dtype;
    e.align = static_cast<u32>(cov::array_align);
    write_pod(tail, s * sizeof(CovSectionEntry), e);
    if (!metadata[s].data.empty())
      std::memcpy(tail.data() + (offsets[s] - table), metadata[s].data.data(), metadata[s].bytes);
  }
  CovSectionEntry blocks{};
  blocks.kind = static_cast<u32>(cov::SectionKind::Blocks);
  blocks.flags = cov::section_required;
  blocks.offset = cov::header_bytes;
  blocks.bytes = table - cov::header_bytes;
  blocks.count = d;
  blocks.dtype = static_cast<u32>(cov::Dtype::U8);
  blocks.align = static_cast<u32>(cov::page_bytes);
  write_pod(tail, 4 * sizeof(CovSectionEntry), blocks);
  CovFileHeader h{};
  h.magic = cov::file_magic;
  h.major = cov::format_major;
  h.minor = cov::format_minor;
  h.header_bytes = cov::header_bytes;
  h.endian_tag = cov::endian_tag;
  h.flags = cov::flag_complete | (spec.f32_exposures ? cov::flag_f32_exposures : 0U) |
            (spec.unsealed ? cov::flag_unsealed : 0U);
  h.file_bytes = file_bytes;
  h.section_count = static_cast<u32>(section_kinds);
  h.date_count = static_cast<u32>(d);
  h.section_table_offset = table;
  h.factor_count = spec.factors();
  h.group_count = spec.groups;
  h.dense_count = spec.dense;
  h.max_rows = w.max_rows;
  h.first_session_ns = w.index.front().as_of_session_ns;
  h.last_session_ns = w.index.back().as_of_session_ns;
  ATX_TRY(h.recipe_sha256, digest_of(text_bytes(spec.recipe_json)));
  ATX_TRY(h.inputs_sha256, digest_of(text_bytes(spec.inputs_json)));
  h.role_sha256 = spec.role_sha256;
  // The root: header[0, 224) || section table || the in-root sections in table order.
  std::vector<std::span<const u8>> parts{pod_bytes(h).first(cov::root_prefix_bytes),
                                         std::span<const u8>(tail).first(table_bytes)};
  for (const auto& m : metadata) parts.push_back(m.data);
  ATX_TRY(h.root_sha256, digest_of(parts));
  CovTrailer trailer{};
  trailer.magic = cov::trailer_magic;
  trailer.file_bytes = file_bytes;
  trailer.root_sha256 = h.root_sha256;
  write_pod(tail, trailer_at - table, trailer);
  std::FILE* f = w.file.get();
  const bool written = write_all(f, tail) && std::fseek(f, 0, SEEK_SET) == 0 &&
                       write_all(f, pod_bytes(h)) && std::fflush(f) == 0 && sync_file(f);
  w.done = true;
  if (!written) {
    w.failed = true;
    return co::Err(co::ErrorCode::IoError, "atx.cov-container: final write or flush");
  }
  if (std::fclose(w.file.release()) != 0)
    return co::Err(co::ErrorCode::IoError, "atx.cov-container: close");
  std::error_code ec;
  if (fs::exists(w.final_path, ec) || ec)
    return co::Err(co::ErrorCode::AlreadyExists,
                   "atx.cov-container: " + w.final_path.string() + " appeared while writing");
  fs::rename(w.partial_path, w.final_path, ec);
  if (ec)
    return co::Err(co::ErrorCode::IoError,
                   "atx.cov-container: rename to " + w.final_path.string() + ": " + ec.message());
  return co::Ok(h.root_sha256);
}

// ---- reader ---------------------------------------------------------------------------------------
struct CovModelFile::Impl {
  atx::tsdb::Mapping map;
  CovFileHeader header{};
  std::span<const u8> bytes;
  std::span<const CovDateIndexEntry> index;
  std::span<const u8> hashes;
  std::string_view meta;
  std::vector<std::string> names;
  VerifyMode mode{VerifyMode::Full};
  // Lazy: 0 unchecked, 1 verified, 2 failed (idempotent; a race hashes a block twice at most).
  std::unique_ptr<std::atomic<u8>[]> state;

  [[nodiscard]] std::span<const u8> block_bytes(usize t) const {
    return bytes.subspan(static_cast<usize>(index[t].block_offset),
                         static_cast<usize>(index[t].block_bytes));
  }
  [[nodiscard]] Sha256Bytes stored_hash(usize t) const {
    Sha256Bytes out{};
    std::memcpy(out.data(), hashes.data() + t * hash_record, out.size());
    return out;
  }
  [[nodiscard]] bool hash_ok(usize t) const {
    const auto digest = digest_of(block_bytes(t));
    return digest && *digest == stored_hash(t);
  }
};

namespace {
struct Range {
  u64 begin{}, end{};
};
co::Status check_header_and_trailer(std::span<const u8> bytes, CovFileHeader& h) {
  const u64 size = bytes.size();
  if (size < u64{cov::header_bytes} + sizeof(CovTrailer))
    return co::Err(parse_error("file shorter than its header and trailer"));
  h = read_pod<CovFileHeader>(bytes, 0);
  if (h.magic != cov::file_magic) return co::Err(parse_error("bad magic"));
  if (h.major != cov::format_major)
    return co::Err(parse_error("major version " + std::to_string(h.major) + " is not 1"));
  if (h.header_bytes != cov::header_bytes || h.endian_tag != cov::endian_tag)
    return co::Err(parse_error("header size or endian tag"));
  if ((h.flags & cov::flag_complete) == 0)
    return co::Err(parse_error("incomplete file (the writer did not finalize)"));
  if ((h.flags & ~cov::known_file_flags) != 0) return co::Err(parse_error("unknown header flag"));
  if (h.file_bytes != size) return co::Err(parse_error("file_bytes != file size (truncated?)"));
  if (!all_zero(h.reserved) || !all_zero(bytes.subspan(sizeof(CovFileHeader),
                                                       cov::header_bytes - sizeof(CovFileHeader))))
    return co::Err(parse_error("header padding not zero"));
  if (h.factor_count != 1U + h.group_count + h.dense_count || h.date_count == 0)
    return co::Err(parse_error("header geometry"));
  const auto t = read_pod<CovTrailer>(bytes, size - sizeof(CovTrailer));
  if (t.magic != cov::trailer_magic || t.file_bytes != size || t.root_sha256 != h.root_sha256 ||
      !all_zero(t.zero))
    return co::Err(parse_error("trailer missing or mismatched (an unfinished write)"));
  return co::Ok();
}

// One known section's record size (count x record == bytes; BLOCKS: count = blocks, its bytes
// tiled by the index); 0 for an unknown kind.
u64 record_bytes(u32 kind) {
  switch (kind) {
  case 1: case 2: case 5: return 1;
  case 3: return index_record;
  case 4: return hash_record;
  default: return 0;
  }
}
constexpr u32 blocks_kind = static_cast<u32>(cov::SectionKind::Blocks);
} // namespace

CovModelFile::CovModelFile(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
CovModelFile::CovModelFile(CovModelFile&&) noexcept = default;
CovModelFile& CovModelFile::operator=(CovModelFile&&) noexcept = default;
CovModelFile::~CovModelFile() = default;

co::Result<CovModelFile> CovModelFile::open(const std::string& path,
                                            std::optional<Sha256Bytes> root_pin, VerifyMode mode,
                                            u64 max_bytes, usize workers) {
  auto impl = std::make_unique<Impl>();
  ATX_TRY(impl->map, atx::tsdb::Mapping::map_file_ro(path, 0, max_bytes));
  impl->bytes = std::span<const u8>(impl->map.base(), impl->map.size());
  impl->mode = mode;
  const auto bytes = impl->bytes;
  auto& h = impl->header;
  ATX_TRY_VOID(check_header_and_trailer(bytes, h));
  const u64 size = bytes.size(), data_end = size - sizeof(CovTrailer);
  // Section table.
  if (h.section_table_offset % cov::page_bytes != 0 || h.section_table_offset < cov::header_bytes ||
      h.section_count == 0 || h.section_count > max_sections ||
      h.section_table_offset + u64{h.section_count} * sizeof(CovSectionEntry) > data_end)
    return co::Err(parse_error("section table out of bounds or misaligned"));
  const auto table = bytes.subspan(static_cast<usize>(h.section_table_offset),
                                   h.section_count * sizeof(CovSectionEntry));
  std::vector<Range> used{{h.section_table_offset, h.section_table_offset + table.size()}};
  std::array<const CovSectionEntry*, section_kinds + 1> known{};
  std::vector<CovSectionEntry> entries(h.section_count);
  std::vector<std::span<const u8>> root_parts{bytes.first(cov::root_prefix_bytes), table};
  u32 previous_kind = 0;
  for (u32 s = 0; s < h.section_count; ++s) {
    auto& e = entries[s];
    e = read_pod<CovSectionEntry>(table, u64{s} * sizeof(CovSectionEntry));
    if (e.kind <= previous_kind || !all_zero(e.zero))
      return co::Err(parse_error("section kinds not ascending or padding not zero"));
    previous_kind = e.kind;
    const u64 record = record_bytes(e.kind);
    const bool is_known = record != 0;
    if (!is_known && (e.flags & cov::section_required) != 0)
      return co::Err(parse_error("unknown required section " + std::to_string(e.kind)));
    const u64 align = e.kind == blocks_kind ? cov::page_bytes : cov::array_align;
    if (e.offset % align != 0 || e.offset < cov::header_bytes || e.offset > data_end ||
        e.bytes > data_end - e.offset)
      return co::Err(parse_error("section out of bounds or misaligned"));
    const u64 elem = is_known ? record : element_bytes(e.dtype);
    if (e.kind != blocks_kind && (elem == 0 || e.count > e.bytes || e.count * elem != e.bytes))
      return co::Err(parse_error("section dtype x count != bytes"));
    used.push_back({e.offset, e.offset + e.bytes});
    if ((e.flags & cov::section_in_root) != 0)
      root_parts.push_back(bytes.subspan(static_cast<usize>(e.offset), static_cast<usize>(e.bytes)));
    if (is_known) known[e.kind] = &e;
  }
  for (usize kind = 1; kind <= section_kinds; ++kind)
    if (known[kind] == nullptr) return co::Err(parse_error("a required section is missing"));
  // No overlap; every byte outside the sections (between the header and the trailer) is zero.
  std::sort(used.begin(), used.end(), [](const Range& a, const Range& b) { return a.begin < b.begin; });
  u64 covered = cov::header_bytes;
  for (const auto& r : used) {
    if (r.begin < covered) return co::Err(parse_error("sections overlap"));
    if (!all_zero(bytes.subspan(static_cast<usize>(covered), static_cast<usize>(r.begin - covered))))
      return co::Err(parse_error("padding between sections not zero"));
    covered = r.end;
  }
  if (!all_zero(bytes.subspan(static_cast<usize>(covered), static_cast<usize>(data_end - covered))))
    return co::Err(parse_error("padding before the trailer not zero"));
  // The root over the metadata, then the caller's pin.
  ATX_TRY(const auto root, digest_of(root_parts));
  if (root != h.root_sha256) return co::Err(parse_error("root SHA-256 mismatch (corrupt metadata)"));
  if (root_pin && *root_pin != root)
    return co::Err(refusal("root SHA-256 " + cov_hex(root) + " != pinned " + cov_hex(*root_pin)));
  // META, names, index, hashes.
  const auto section = [&](cov::SectionKind kind) {
    const auto* e = known[static_cast<usize>(kind)];
    return bytes.subspan(static_cast<usize>(e->offset), static_cast<usize>(e->bytes));
  };
  const auto meta = section(cov::SectionKind::MetaJson);
  impl->meta = std::string_view(reinterpret_cast<const char*>(meta.data()), meta.size());
  const auto names = section(cov::SectionKind::FactorNames);
  std::string current;
  for (const u8 c : names) {
    if (c != 0) { current.push_back(static_cast<char>(c)); continue; }
    if (current.empty()) return co::Err(parse_error("empty factor name"));
    impl->names.push_back(std::move(current));
    current.clear();
  }
  if (!current.empty() || impl->names.size() != h.factor_count)
    return co::Err(parse_error("factor names: K NUL-terminated names expected"));
  const u64 d = h.date_count;
  const auto* index_section = known[static_cast<usize>(cov::SectionKind::DateIndex)];
  const auto* hash_section = known[static_cast<usize>(cov::SectionKind::BlockHashes)];
  const auto* blocks_section = known[static_cast<usize>(cov::SectionKind::Blocks)];
  if (index_section->count != d || hash_section->count != d || blocks_section->count != d ||
      blocks_section->offset != cov::header_bytes)
    return co::Err(parse_error("index, hashes or blocks do not hold date_count entries"));
  // SAFETY: the DATE_INDEX section is 64-byte aligned inside a page-aligned mapping and holds
  // exactly d records of the trivially copyable, standard-layout CovDateIndexEntry.
  impl->index = std::span<const CovDateIndexEntry>(
      reinterpret_cast<const CovDateIndexEntry*>(bytes.data() + index_section->offset),
      static_cast<usize>(d));
  impl->hashes = section(cov::SectionKind::BlockHashes);
  u64 next_block = blocks_section->offset;
  for (usize t = 0; t < d; ++t) {
    const auto& e = impl->index[t];
    if ((t > 0 && e.as_of_session_ns <= impl->index[t - 1].as_of_session_ns) ||
        e.data_cutoff_ns > e.as_of_session_ns || e.history_first_ns > e.data_cutoff_ns)
      return co::Err(parse_error("date index: as_of not ascending or a cutoff after as_of"));
    if (e.block_offset != next_block || e.block_bytes == 0 || e.block_bytes % cov::page_bytes != 0)
      return co::Err(parse_error("date index: blocks do not tile the BLOCKS section"));
    next_block += e.block_bytes;
    if (e.priced_rows > e.rows || e.rows > h.max_rows || (e.flags & ~cov::known_date_flags) != 0 ||
        e.zero != 0 || e.unforecast_factors > h.factor_count)
      return co::Err(parse_error("date index: rows, flags or padding"));
  }
  if (next_block != blocks_section->offset + blocks_section->bytes ||
      h.first_session_ns != impl->index.front().as_of_session_ns ||
      h.last_session_ns != impl->index.back().as_of_session_ns)
    return co::Err(parse_error("date index: blocks or first/last session disagree with the header"));
  impl->state = std::make_unique<std::atomic<u8>[]>(static_cast<usize>(d));
  if (mode == VerifyMode::Full) {
    const usize hardware = std::max<usize>(1, std::thread::hardware_concurrency());
    const usize pool = std::min<usize>(workers ? workers : std::min(default_workers, hardware),
                                       static_cast<usize>(d));
    // Fixed partition [w d / pool, (w+1) d / pool); each worker records its first failure, the
    // smallest over workers is reported: the same answer for any pool size.
    std::vector<usize> first_bad(pool, static_cast<usize>(d));
    const Impl& reader = *impl;
    const auto work = [&reader, &first_bad, d, pool](usize w) {
      const usize lo = static_cast<usize>(w * d / pool), hi = static_cast<usize>((w + 1) * d / pool);
      for (usize t = lo; t < hi; ++t) {
        const bool ok = reader.hash_ok(t);
        reader.state[t].store(ok ? u8{1} : u8{2}, std::memory_order_release);
        if (!ok && first_bad[w] == d) first_bad[w] = t;
      }
    };
    try {
      std::vector<std::jthread> threads;
      threads.reserve(pool > 0 ? pool - 1 : 0);
      for (usize w = 1; w < pool; ++w) threads.emplace_back(work, w);
      work(0);
    } catch (const std::system_error& e) {
      return co::Err(co::ErrorCode::Unavailable,
                     std::string("atx.cov-container: verification threads: ") + e.what());
    }
    const usize bad = *std::min_element(first_bad.begin(), first_bad.end());
    if (bad < d)
      return co::Err(parse_error("block " + std::to_string(bad) + " (session " +
                                 std::to_string(impl->index[bad].as_of_session_ns) +
                                 ") fails its SHA-256"));
  }
  return co::Ok(CovModelFile(std::move(impl)));
}

const CovFileHeader& CovModelFile::header() const noexcept { return impl_->header; }
usize CovModelFile::date_count() const noexcept { return impl_->index.size(); }
u32 CovModelFile::factors() const noexcept { return impl_->header.factor_count; }
u32 CovModelFile::groups() const noexcept { return impl_->header.group_count; }
u32 CovModelFile::dense() const noexcept { return impl_->header.dense_count; }
bool CovModelFile::f32_exposures() const noexcept {
  return (impl_->header.flags & cov::flag_f32_exposures) != 0;
}
bool CovModelFile::unsealed() const noexcept {
  return (impl_->header.flags & cov::flag_unsealed) != 0;
}
VerifyMode CovModelFile::mode() const noexcept { return impl_->mode; }
u64 CovModelFile::file_bytes() const noexcept { return impl_->header.file_bytes; }
std::span<const CovDateIndexEntry> CovModelFile::index() const noexcept { return impl_->index; }
std::string_view CovModelFile::meta_json() const noexcept { return impl_->meta; }
const std::vector<std::string>& CovModelFile::factor_names() const noexcept { return impl_->names; }
const Sha256Bytes& CovModelFile::root_sha256() const noexcept { return impl_->header.root_sha256; }
const Sha256Bytes& CovModelFile::recipe_sha256() const noexcept {
  return impl_->header.recipe_sha256;
}
const Sha256Bytes& CovModelFile::role_sha256() const noexcept { return impl_->header.role_sha256; }
const Sha256Bytes& CovModelFile::inputs_sha256() const noexcept {
  return impl_->header.inputs_sha256;
}

co::Result<Sha256Bytes> CovModelFile::block_sha256(usize t) const {
  if (t >= impl_->index.size()) return co::Err(co::ErrorCode::OutOfRange, "atx.cov-container: block");
  return co::Ok(impl_->stored_hash(t));
}

co::Result<CovBlockView> CovModelFile::view(usize t) const {
  const auto& r = *impl_;
  if (t >= r.index.size())
    return co::Err(refusal("no block " + std::to_string(t) + " (" +
                           std::to_string(r.index.size()) + " blocks)"));
  u8 state = r.state[t].load(std::memory_order_acquire);
  if (state == 0) {
    state = r.hash_ok(t) ? u8{1} : u8{2};
    r.state[t].store(state, std::memory_order_release);
  }
  if (state != 1) return co::Err(parse_error("block " + std::to_string(t) + " fails its SHA-256"));
  ATX_TRY(auto v, parse_cov_block(r.block_bytes(t), r.header.factor_count, r.header.group_count,
                                  r.header.dense_count, f32_exposures()));
  const auto& e = r.index[t];
  if (v.as_of != e.as_of_session_ns || v.cutoff != e.data_cutoff_ns || v.rows != e.rows ||
      v.priced != e.priced_rows)
    return co::Err(parse_error("block " + std::to_string(t) + " header disagrees with the index"));
  return co::Ok(v);
}

co::Result<CovBlockView> CovModelFile::exact(i64 session) const {
  const auto idx = index();
  const auto it = std::lower_bound(idx.begin(), idx.end(), session,
      [](const CovDateIndexEntry& e, i64 s) { return e.as_of_session_ns < s; });
  if (it == idx.end() || it->as_of_session_ns != session)
    return co::Err(refusal("no block for session " + std::to_string(session)));
  return view(static_cast<usize>(it - idx.begin()));
}

std::optional<usize> CovModelFile::index_at_or_before(i64 session) const noexcept {
  const auto idx = index();
  // The last block whose as_of (hence cutoff <= as_of) is at or before the session.
  const auto it = std::upper_bound(idx.begin(), idx.end(), session,
      [](i64 s, const CovDateIndexEntry& e) { return s < e.as_of_session_ns; });
  if (it == idx.begin()) return std::nullopt;
  const usize t = static_cast<usize>(it - idx.begin()) - 1;
  if (idx[t].data_cutoff_ns > session) return std::nullopt; // unreachable: cutoff <= as_of
  return t;
}

co::Result<CovBlockView> CovModelFile::at_or_before(i64 session) const {
  const auto t = index_at_or_before(session);
  if (!t) return co::Err(refusal("no block at or before session " + std::to_string(session)));
  return view(*t);
}

CovDifference first_difference(const CovModelFile& a, const CovModelFile& b, i64 through) {
  CovDifference out;
  if (a.factors() != b.factors() || a.groups() != b.groups() || a.dense() != b.dense() ||
      a.f32_exposures() != b.f32_exposures()) {
    out.differs = true;
    out.index = 0;
    out.session = std::min(a.header().first_session_ns, b.header().first_session_ns);
    out.what = "geometry";
    return out;
  }
  const auto ia = a.index(), ib = b.index();
  const usize bound = std::max(ia.size(), ib.size());
  for (usize t = 0; t < bound; ++t) {
    const bool in_a = t < ia.size() && ia[t].as_of_session_ns <= through;
    const bool in_b = t < ib.size() && ib[t].as_of_session_ns <= through;
    if (!in_a && !in_b) break;
    out.index = t;
    if (in_a != in_b) {
      out.differs = true;
      out.session = in_a ? ia[t].as_of_session_ns : ib[t].as_of_session_ns;
      out.what = "missing block";
      return out;
    }
    if (ia[t].as_of_session_ns != ib[t].as_of_session_ns) {
      out.differs = true;
      out.session = std::min(ia[t].as_of_session_ns, ib[t].as_of_session_ns);
      out.what = "sessions";
      return out;
    }
    if (*a.block_sha256(t) != *b.block_sha256(t)) {
      out.differs = true;
      out.session = ia[t].as_of_session_ns;
      out.what = "block";
      return out;
    }
  }
  out.index = 0;
  return out;
}

std::string cov_hex(const Sha256Bytes& digest) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string out(64, '0');
  for (usize i = 0; i < digest.size(); ++i) {
    out[2 * i] = digits[digest[i] >> 4U];
    out[2 * i + 1] = digits[digest[i] & 15U];
  }
  return out;
}

std::optional<Sha256Bytes> cov_parse_hex(std::string_view hex) {
  if (hex.size() != 64) return std::nullopt;
  const auto value = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  Sha256Bytes out{};
  for (usize i = 0; i < out.size(); ++i) {
    const int hi = value(hex[2 * i]), lo = value(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    out[i] = static_cast<u8>(hi * 16 + lo);
  }
  return out;
}

} // namespace atx::engine::risk
