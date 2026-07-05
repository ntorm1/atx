#include "atx/vol/surface_archive.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "atx/core/bit.hpp"   // next_pow2, is_pow2
#include "atx/core/hash.hpp"  // hash_bytes

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

// The slice structs are serialized verbatim (std::memcpy to / from the byte
// buffer), so they must be trivially copyable for the round-trip to be
// well-defined. A future field change that breaks this is a compile error.
static_assert(std::is_trivially_copyable_v<EssviParams>,
              "EssviParams must be trivially copyable for byte serialization");
static_assert(std::is_trivially_copyable_v<SviParams>,
              "SviParams must be trivially copyable for byte serialization");

namespace {

// ── CRC-32C (Castagnoli, reflected poly 0x82F63B78) ──────────────────────
//
// Hand-rolled: atx-core exposes wyhash (hash.hpp) but no CRC. This mirrors the
// C library's ats_crc32c (init 0xFFFFFFFF, final XOR 0xFFFFFFFF). Only internal
// round-trip consistency matters — the archive is not shared with the C
// library — so a standard table-driven CRC-32C is sufficient and faithful.

[[nodiscard]] constexpr std::array<std::uint32_t, 256> make_crc32c_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t n = 0; n < 256; ++n) {
    std::uint32_t c = n;
    for (int k = 0; k < 8; ++k) {
      c = ((c & 1u) != 0u) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
    }
    table[n] = c;
  }
  return table;
}

inline constexpr std::array<std::uint32_t, 256> kCrc32cTable = make_crc32c_table();

// Continue a CRC-32C over [p, p+n). `crc` is the running (un-finalized) state.
[[nodiscard]] std::uint32_t crc32c_update(std::uint32_t crc, const std::byte* p,
                                          std::size_t n) noexcept {
  for (std::size_t i = 0; i < n; ++i) {
    const auto byte = static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[i]));
    crc = kCrc32cTable[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
  }
  return crc;
}

// One-shot CRC-32C with the standard init/final XOR applied.
[[nodiscard]] std::uint32_t crc32c(const std::byte* p, std::size_t n) noexcept {
  return crc32c_update(0xFFFFFFFFu, p, n) ^ 0xFFFFFFFFu;
}

// ── Small helpers ────────────────────────────────────────────────────────

constexpr char kArchiveMagic[8] = {'A', 'T', 'S', 'V', 'S', 'A', '0', '2'};
constexpr char kBlobMagic[8] = {'A', 'T', 'S', 'V', 'S', 'F', 'C', '2'};

[[nodiscard]] constexpr std::uint64_t align_up(std::uint64_t v, std::uint64_t a) noexcept {
  return (v + (a - 1u)) & ~(a - 1u);
}

[[nodiscard]] std::byte* buf_at(std::vector<std::byte>& b, std::uint64_t off) noexcept {
  return b.data() + static_cast<std::size_t>(off);
}
[[nodiscard]] const std::byte* buf_at(const std::vector<std::byte>& b,
                                      std::uint64_t off) noexcept {
  return b.data() + static_cast<std::size_t>(off);
}

// Compile-time-constant fingerprint of the on-disk layout. Folds the sizeof of
// every serialized record so a reader built against a different struct shape
// rejects the file (ParseError) instead of mis-reading bytes. Mirrors the C's
// archive_schema_hash (FNV prime, golden-ratio seed).
[[nodiscard]] std::uint64_t schema_hash() noexcept {
  constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;
  std::uint64_t h = 0x9e3779b97f4a7c15ull;
  h ^= static_cast<std::uint64_t>(sizeof(ArchiveHeader)) * kFnvPrime;
  h ^= static_cast<std::uint64_t>(sizeof(ArchiveIndexSlot)) * kFnvPrime;
  h ^= static_cast<std::uint64_t>(sizeof(ArchiveDirEntry)) * kFnvPrime;
  h ^= static_cast<std::uint64_t>(sizeof(SurfaceBlobHeader)) * kFnvPrime;
  h ^= static_cast<std::uint64_t>(sizeof(EssviParams)) * kFnvPrime;
  h ^= static_cast<std::uint64_t>(sizeof(SviParams)) * kFnvPrime;
  return h;
}

// CRC-32C over a header with its own checksum field zeroed (the field cannot
// cover itself). Takes the header by value so the caller's copy is untouched.
[[nodiscard]] std::uint32_t header_crc(ArchiveHeader h) noexcept {
  h.header_crc32c = 0;
  std::array<std::byte, sizeof(ArchiveHeader)> bytes{};
  std::memcpy(bytes.data(), &h, sizeof h);
  return crc32c(bytes.data(), bytes.size());
}

// ASCII upper-case + truncate to kArchiveSymbolMax; zero-pads the tail. Returns
// the canonical length. Matches the C's canonicalize_symbol.
[[nodiscard]] std::uint16_t canonicalize(std::string_view src,
                                         std::array<char, kArchiveSymbolMax>& dst) noexcept {
  const std::size_t n = std::min(src.size(), kArchiveSymbolMax);
  for (std::size_t i = 0; i < n; ++i) {
    char c = src[i];
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
    dst[i] = c;
  }
  for (std::size_t i = n; i < kArchiveSymbolMax; ++i) {
    dst[i] = char{0};
  }
  return static_cast<std::uint16_t>(n);
}

[[nodiscard]] std::uint64_t slice_elem_size(Parametrization p) noexcept {
  return (p == Parametrization::Essvi) ? sizeof(EssviParams) : sizeof(SviParams);
}

[[nodiscard]] bool serializable_param(Parametrization p) noexcept {
  return p == Parametrization::Essvi || p == Parametrization::Svi ||
         p == Parametrization::SviMm;
}

[[nodiscard]] std::int64_t wall_clock_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// One planned surface blob. Populated during the pre-pass, sorted by canonical
// symbol for a deterministic layout, then materialized into the buffer.
struct BlobPlan {
  std::array<char, kArchiveSymbolMax> symbol{};
  std::uint16_t symbol_len{};
  std::uint64_t symbol_hash{};
  Parametrization param{Parametrization::Essvi};
  std::uint16_t n_slices{};
  std::uint32_t uid{};
  std::uint64_t elem_size{};
  std::uint64_t symbol_offset{};
  std::uint64_t symbol_size{};
  std::uint64_t slices_offset{};
  std::uint64_t slices_size{};
  std::uint64_t blob_size{};
  std::uint64_t file_offset{};
  std::uint32_t crc32c{};
  std::size_t item_index{};
  std::size_t slot_index{};
};

// Canonical-symbol comparator (memcmp of the shorter prefix, then length) —
// gives a deterministic layout independent of caller order. Mirrors the C's
// blob_plan_cmp.
[[nodiscard]] bool plan_less(const BlobPlan& a, const BlobPlan& b) noexcept {
  const std::uint16_t n = std::min(a.symbol_len, b.symbol_len);
  const int c = std::memcmp(a.symbol.data(), b.symbol.data(), n);
  if (c != 0) {
    return c < 0;
  }
  return a.symbol_len < b.symbol_len;
}

}  // namespace

// ── Writer ───────────────────────────────────────────────────────────────

Result<std::vector<std::byte>>
write_surface_archive(std::span<const SurfaceArchiveItem> items,
                      const SurfaceArchiveWriteOpts& opts) {
  if (items.empty()) {
    return Err(ErrorCode::InvalidArgument, "write_surface_archive: no items");
  }
  if (items.size() > 0xFFFFFFFFull) {
    return Err(ErrorCode::InvalidArgument, "write_surface_archive: too many items");
  }
  if (opts.lookup_load_pct == 0 || opts.lookup_load_pct > 100) {
    return Err(ErrorCode::InvalidArgument,
               "write_surface_archive: lookup_load_pct must be in (0, 100]");
  }
  const std::uint64_t array_align =
      opts.array_alignment != 0 ? opts.array_alignment : kArchiveArrayAlign;
  const std::uint64_t blob_align =
      opts.blob_alignment != 0 ? opts.blob_alignment : kArchiveBlobAlign;

  const auto n_items = static_cast<std::uint32_t>(items.size());

  // 1. Plan + validate every item.
  std::vector<BlobPlan> plans;
  plans.reserve(n_items);
  for (std::size_t i = 0; i < items.size(); ++i) {
    const SurfaceArchiveItem& it = items[i];
    if (it.surface == nullptr) {
      return Err(ErrorCode::InvalidArgument, "write_surface_archive: null surface");
    }
    if (it.symbol.empty()) {
      return Err(ErrorCode::InvalidArgument, "write_surface_archive: empty symbol");
    }
    const Parametrization p = it.surface->param();
    if (!serializable_param(p)) {
      return Err(ErrorCode::ParseError,
                 "write_surface_archive: parametrization has no serializable slices");
    }
    const std::size_t n = it.surface->n_slices();
    if (n == 0) {
      return Err(ErrorCode::InvalidArgument, "write_surface_archive: surface has no slices");
    }
    if (n > 0xFFFFu) {
      return Err(ErrorCode::InvalidArgument, "write_surface_archive: too many slices");
    }

    BlobPlan plan;
    plan.item_index = i;
    plan.symbol_len = canonicalize(it.symbol, plan.symbol);
    if (plan.symbol_len == 0) {
      return Err(ErrorCode::InvalidArgument, "write_surface_archive: empty canonical symbol");
    }
    plan.symbol_hash = atx::core::hash_bytes(plan.symbol.data(), plan.symbol_len);
    plan.param = p;
    plan.n_slices = static_cast<std::uint16_t>(n);
    plan.uid = it.surface->uid();
    plan.elem_size = slice_elem_size(p);

    std::uint64_t cur = sizeof(SurfaceBlobHeader);
    plan.symbol_offset = cur;
    plan.symbol_size = plan.symbol_len;
    cur = align_up(cur + plan.symbol_size, array_align);
    plan.slices_offset = cur;
    plan.slices_size = static_cast<std::uint64_t>(plan.n_slices) * plan.elem_size;
    cur = align_up(cur + plan.slices_size, array_align);
    plan.blob_size = cur;

    plans.push_back(plan);
  }

  // 2. Deterministic order by canonical symbol.
  std::sort(plans.begin(), plans.end(), plan_less);

  // 3. Geometry.
  const std::uint64_t load = opts.lookup_load_pct;
  const std::uint64_t want_slots = (static_cast<std::uint64_t>(n_items) * 100ull + load - 1ull) / load;
  std::uint32_t lookup_slots = atx::core::next_pow2(static_cast<std::uint32_t>(want_slots));
  if (lookup_slots < 8u) {
    lookup_slots = 8u;
  }
  const std::uint64_t lookup_offset = align_up(sizeof(ArchiveHeader), 64u);
  const std::uint64_t lookup_bytes = static_cast<std::uint64_t>(lookup_slots) * sizeof(ArchiveIndexSlot);
  const std::uint64_t directory_offset = lookup_offset + lookup_bytes;
  const std::uint64_t dir_bytes = static_cast<std::uint64_t>(n_items) * sizeof(ArchiveDirEntry);
  const std::uint64_t data_offset = align_up(directory_offset + dir_bytes, blob_align);

  std::uint64_t cursor = data_offset;
  for (BlobPlan& plan : plans) {
    plan.file_offset = align_up(cursor, blob_align);
    cursor = plan.file_offset + plan.blob_size;
  }
  const std::uint64_t file_size = cursor;

  // 4. Build the lookup table (open-addressed; duplicate canonical symbol is an
  //    AlreadyExists) and the directory.
  std::vector<ArchiveIndexSlot> lookup(lookup_slots);
  std::vector<ArchiveDirEntry> directory(n_items);
  const std::uint64_t mask = static_cast<std::uint64_t>(lookup_slots) - 1ull;
  for (std::size_t idx = 0; idx < plans.size(); ++idx) {
    BlobPlan& plan = plans[idx];
    std::uint64_t i = plan.symbol_hash & mask;
    bool placed = false;
    for (std::uint32_t step = 0; step < lookup_slots; ++step) {
      ArchiveIndexSlot& s = lookup[static_cast<std::size_t>(i)];
      if (s.flags == kArchiveSlotEmpty) {
        s.symbol_hash = plan.symbol_hash;
        s.surface_offset = plan.file_offset;
        s.surface_size = plan.blob_size;
        s.surface_crc32c = 0;  // patched after the blob is materialized
        s.uid = plan.uid;
        s.symbol_len = plan.symbol_len;
        s.flags = kArchiveSlotOccupied;
        std::memcpy(s.symbol, plan.symbol.data(), plan.symbol_len);
        plan.slot_index = static_cast<std::size_t>(i);
        placed = true;
        break;
      }
      if (s.symbol_hash == plan.symbol_hash && s.symbol_len == plan.symbol_len &&
          std::memcmp(s.symbol, plan.symbol.data(), plan.symbol_len) == 0) {
        return Err(ErrorCode::AlreadyExists,
                   "write_surface_archive: duplicate canonical symbol");
      }
      i = (i + 1ull) & mask;
    }
    if (!placed) {
      return Err(ErrorCode::Internal, "write_surface_archive: lookup table full");
    }

    ArchiveDirEntry& de = directory[idx];
    de.surface_offset = plan.file_offset;
    de.surface_size = plan.blob_size;
    de.symbol_hash = plan.symbol_hash;
    de.uid = plan.uid;
    de.symbol_len = plan.symbol_len;
    de.param = static_cast<std::uint16_t>(plan.param);
    de.n_slices = plan.n_slices;
    std::memcpy(de.symbol, plan.symbol.data(), plan.symbol_len);
  }

  // 5. Materialize the buffer: header placeholder (zeros), blobs, then lookup +
  //    directory, then finalize checksums.
  std::vector<std::byte> buffer(static_cast<std::size_t>(file_size));

  for (BlobPlan& plan : plans) {
    const VolSurface& surf = *items[plan.item_index].surface;
    std::byte* base = buf_at(buffer, plan.file_offset);

    // Payload: symbol bytes, then the slice array (copied verbatim — the slice
    // structs are trivially copyable, so this round-trips bit-for-bit).
    std::memcpy(base + static_cast<std::size_t>(plan.symbol_offset), plan.symbol.data(),
                plan.symbol_len);
    if (plan.slices_size > 0) {
      if (plan.param == Parametrization::Essvi) {
        const std::span<const EssviParams> sl = surf.essvi_slices();
        std::memcpy(base + static_cast<std::size_t>(plan.slices_offset), sl.data(),
                    static_cast<std::size_t>(plan.slices_size));
      } else {
        const std::span<const SviParams> sl = surf.svi_slices();
        std::memcpy(base + static_cast<std::size_t>(plan.slices_offset), sl.data(),
                    static_cast<std::size_t>(plan.slices_size));
      }
    }

    // Header (written after the payload so payload_crc32c is well-defined).
    SurfaceBlobHeader bh;
    std::memcpy(bh.magic, kBlobMagic, 8);
    bh.major = kArchiveMajor;
    bh.minor = kArchiveMinor;
    bh.param = static_cast<std::uint16_t>(plan.param);
    bh.uid = plan.uid;
    bh.n_slices = plan.n_slices;
    bh.slices_offset = plan.slices_offset;
    bh.slices_size = plan.slices_size;
    bh.symbol_offset = plan.symbol_offset;
    bh.symbol_size = plan.symbol_size;
    bh.blob_header_size = static_cast<std::uint32_t>(sizeof(SurfaceBlobHeader));
    bh.blob_size = plan.blob_size;
    bh.fit_ts_ns = surf.fit_ts_ns();
    const VolSurface::Diagnostics& diag = surf.diagnostics();
    bh.rmse_vol = diag.rmse_vol;
    bh.max_residual_vol = diag.max_residual_vol;
    bh.n_quotes_used = diag.n_quotes_used;
    bh.n_quotes_dropped = diag.n_quotes_dropped;
    bh.payload_crc32c = crc32c(base + sizeof(SurfaceBlobHeader),
                               static_cast<std::size_t>(plan.blob_size - sizeof(SurfaceBlobHeader)));
    std::memcpy(base, &bh, sizeof bh);

    // Whole-blob CRC into the owning lookup slot.
    plan.crc32c = crc32c(base, static_cast<std::size_t>(plan.blob_size));
    lookup[plan.slot_index].surface_crc32c = plan.crc32c;
  }

  if (lookup_bytes > 0) {
    std::memcpy(buf_at(buffer, lookup_offset), lookup.data(), static_cast<std::size_t>(lookup_bytes));
  }
  if (dir_bytes > 0) {
    std::memcpy(buf_at(buffer, directory_offset), directory.data(), static_cast<std::size_t>(dir_bytes));
  }

  // metadata CRC = CRC-32C over (lookup ‖ directory).
  std::uint32_t meta = crc32c_update(0xFFFFFFFFu, buf_at(buffer, lookup_offset),
                                     static_cast<std::size_t>(lookup_bytes));
  meta = crc32c_update(meta, buf_at(buffer, directory_offset),
                       static_cast<std::size_t>(dir_bytes)) ^
         0xFFFFFFFFu;

  // 6. Header.
  ArchiveHeader hdr;
  std::memcpy(hdr.magic, kArchiveMagic, 8);
  hdr.major = kArchiveMajor;
  hdr.minor = kArchiveMinor;
  hdr.header_size = static_cast<std::uint16_t>(sizeof(ArchiveHeader));
  hdr.endian = 1;
  hdr.pointer_bits = 64;
  hdr.alignment_log2 = 12;
  hdr.flags = opts.flags;
  hdr.file_size = file_size;
  hdr.created_ts_ns = opts.created_ts_ns != 0 ? static_cast<std::uint64_t>(opts.created_ts_ns)
                                              : static_cast<std::uint64_t>(wall_clock_ns());
  hdr.schema_hash = schema_hash();
  hdr.writer_version_hash = 0;
  hdr.surface_count = n_items;
  hdr.lookup_slot_count = lookup_slots;
  hdr.lookup_offset = lookup_offset;
  hdr.directory_offset = directory_offset;
  hdr.data_offset = data_offset;
  hdr.index_slot_size = static_cast<std::uint32_t>(sizeof(ArchiveIndexSlot));
  hdr.dir_entry_size = static_cast<std::uint32_t>(sizeof(ArchiveDirEntry));
  hdr.surface_blob_header_size = static_cast<std::uint32_t>(sizeof(SurfaceBlobHeader));
  hdr.metadata_crc32c = meta;
  hdr.header_crc32c = header_crc(hdr);
  std::memcpy(buf_at(buffer, 0), &hdr, sizeof hdr);

  return Ok(std::move(buffer));
}

Status write_surface_archive_file(std::string_view path,
                                  std::span<const SurfaceArchiveItem> items,
                                  const SurfaceArchiveWriteOpts& opts) {
  auto built = write_surface_archive(items, opts);
  if (!built) {
    return tl::unexpected<atx::core::Error>(std::move(built).error());
  }
  const std::vector<std::byte>& buffer = *built;

  const std::filesystem::path dst{std::string(path)};
  std::filesystem::path tmp = dst;
  tmp += ".tmp";

  {
    std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
    if (!os) {
      return Err(ErrorCode::IoError, "write_surface_archive_file: cannot open temp file");
    }
    // SAFETY: std::byte may alias any object; reading its representation through
    // char* for stream I/O is well-defined.
    os.write(reinterpret_cast<const char*>(buffer.data()),
             static_cast<std::streamsize>(buffer.size()));
    if (!os) {
      std::error_code ec;
      std::filesystem::remove(tmp, ec);
      return Err(ErrorCode::IoError, "write_surface_archive_file: write failed");
    }
  }

  std::error_code ec;
  std::filesystem::rename(tmp, dst, ec);
  if (ec) {
    std::error_code ec2;
    std::filesystem::remove(tmp, ec2);
    return Err(ErrorCode::IoError, "write_surface_archive_file: rename failed");
  }
  return Ok();
}

// ── Reader ───────────────────────────────────────────────────────────────

Result<SurfaceArchive> SurfaceArchive::open(std::vector<std::byte> bytes) {
  if (bytes.size() < sizeof(ArchiveHeader)) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: shorter than header");
  }

  SurfaceArchive a;
  a.buffer_ = std::move(bytes);
  const std::vector<std::byte>& buf = a.buffer_;

  ArchiveHeader h;
  std::memcpy(&h, buf.data(), sizeof h);

  if (std::memcmp(h.magic, kArchiveMagic, 8) != 0) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: bad magic");
  }
  if (h.major != kArchiveMajor) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: unsupported major version");
  }
  if (h.endian != 1) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: non-little-endian archive");
  }
  if (h.pointer_bits != 64) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: unsupported pointer width");
  }
  if (h.header_size != sizeof(ArchiveHeader) || h.index_slot_size != sizeof(ArchiveIndexSlot) ||
      h.dir_entry_size != sizeof(ArchiveDirEntry) ||
      h.surface_blob_header_size != sizeof(SurfaceBlobHeader)) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: record size mismatch");
  }
  if (h.schema_hash != schema_hash()) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: schema hash mismatch");
  }
  if (h.file_size != buf.size()) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: file size mismatch");
  }
  if (h.lookup_slot_count == 0 || !atx::core::is_pow2(h.lookup_slot_count)) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: lookup slot count not a power of two");
  }

  const std::uint64_t lookup_bytes =
      static_cast<std::uint64_t>(h.lookup_slot_count) * h.index_slot_size;
  const std::uint64_t dir_bytes =
      static_cast<std::uint64_t>(h.surface_count) * h.dir_entry_size;
  if (h.lookup_offset < sizeof(ArchiveHeader)) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: lookup overlaps header");
  }
  if (h.lookup_offset > h.file_size || lookup_bytes > h.file_size - h.lookup_offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: lookup out of bounds");
  }
  if (h.lookup_offset + lookup_bytes > h.directory_offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: lookup overlaps directory");
  }
  if (h.directory_offset > h.file_size || dir_bytes > h.file_size - h.directory_offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: directory out of bounds");
  }
  if (h.directory_offset + dir_bytes > h.data_offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: directory overlaps data");
  }
  if (h.data_offset > h.file_size) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: data offset out of bounds");
  }

  if (header_crc(h) != h.header_crc32c) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: header checksum mismatch");
  }

  // metadata CRC over (lookup ‖ directory).
  std::uint32_t meta = crc32c_update(0xFFFFFFFFu, buf_at(buf, h.lookup_offset),
                                     static_cast<std::size_t>(lookup_bytes));
  meta = crc32c_update(meta, buf_at(buf, h.directory_offset),
                       static_cast<std::size_t>(dir_bytes)) ^
         0xFFFFFFFFu;
  if (meta != h.metadata_crc32c) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::open: metadata checksum mismatch");
  }

  a.header_ = h;
  a.lookup_.resize(h.lookup_slot_count);
  if (lookup_bytes > 0) {
    std::memcpy(a.lookup_.data(), buf_at(buf, h.lookup_offset), static_cast<std::size_t>(lookup_bytes));
  }
  a.directory_.resize(h.surface_count);
  if (dir_bytes > 0) {
    std::memcpy(a.directory_.data(), buf_at(buf, h.directory_offset), static_cast<std::size_t>(dir_bytes));
  }

  // Every directory entry must sit fully inside the data region.
  for (const ArchiveDirEntry& de : a.directory_) {
    if (de.surface_offset < h.data_offset) {
      return Err(ErrorCode::ParseError, "SurfaceArchive::open: directory entry precedes data");
    }
    if (de.surface_offset > h.file_size || de.surface_size > h.file_size - de.surface_offset) {
      return Err(ErrorCode::ParseError, "SurfaceArchive::open: directory entry out of bounds");
    }
  }

  return Ok(std::move(a));
}

Result<SurfaceArchive> SurfaceArchive::open_file(std::string_view path) {
  const std::filesystem::path p{std::string(path)};
  std::error_code ec;
  if (!std::filesystem::exists(p, ec) || ec) {
    return Err(ErrorCode::NotFound, "SurfaceArchive::open_file: file not found");
  }
  std::ifstream is(p, std::ios::binary | std::ios::ate);
  if (!is) {
    return Err(ErrorCode::IoError, "SurfaceArchive::open_file: cannot open file");
  }
  const std::streamsize size = is.tellg();
  if (size < 0) {
    return Err(ErrorCode::IoError, "SurfaceArchive::open_file: cannot size file");
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  is.seekg(0);
  // SAFETY: char* aliasing of std::byte storage is well-defined for stream I/O.
  is.read(reinterpret_cast<char*>(bytes.data()), size);
  if (is.gcount() != size) {
    return Err(ErrorCode::IoError, "SurfaceArchive::open_file: short read");
  }
  return open(std::move(bytes));
}

const ArchiveIndexSlot* SurfaceArchive::find_slot(std::string_view symbol) const noexcept {
  if (lookup_.empty()) {
    return nullptr;
  }
  std::array<char, kArchiveSymbolMax> canon{};
  const std::uint16_t len = canonicalize(symbol, canon);
  if (len == 0) {
    return nullptr;
  }
  const std::uint64_t h = atx::core::hash_bytes(canon.data(), len);
  const std::uint64_t mask = static_cast<std::uint64_t>(lookup_.size()) - 1ull;
  std::uint64_t i = h & mask;
  for (std::size_t step = 0; step < lookup_.size(); ++step) {
    const ArchiveIndexSlot& s = lookup_[static_cast<std::size_t>(i)];
    if (s.flags == kArchiveSlotEmpty) {
      return nullptr;
    }
    if (s.symbol_hash == h && s.symbol_len == len &&
        std::memcmp(s.symbol, canon.data(), len) == 0) {
      return &s;
    }
    i = (i + 1ull) & mask;
  }
  return nullptr;
}

Result<ArchiveDirEntry> SurfaceArchive::find(std::string_view symbol) const {
  const ArchiveIndexSlot* s = find_slot(symbol);
  if (s == nullptr) {
    return Err(ErrorCode::NotFound, "SurfaceArchive::find: symbol not present");
  }
  ArchiveDirEntry de;
  de.surface_offset = s->surface_offset;
  de.surface_size = s->surface_size;
  de.symbol_hash = s->symbol_hash;
  de.uid = s->uid;
  de.symbol_len = s->symbol_len;
  std::memcpy(de.symbol, s->symbol, s->symbol_len);
  return de;
}

Result<VolSurface> SurfaceArchive::reconstruct(std::uint64_t offset, std::uint64_t size,
                                               std::uint32_t expected_crc) const {
  if (size < sizeof(SurfaceBlobHeader)) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: blob smaller than header");
  }
  if (offset > buffer_.size() || size > buffer_.size() - offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: blob out of bounds");
  }
  const std::byte* base = buf_at(buffer_, offset);

  if (crc32c(base, static_cast<std::size_t>(size)) != expected_crc) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: blob checksum mismatch");
  }

  SurfaceBlobHeader bh;
  std::memcpy(&bh, base, sizeof bh);
  if (std::memcmp(bh.magic, kBlobMagic, 8) != 0) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: bad blob magic");
  }
  if (bh.major != kArchiveMajor) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: unsupported blob version");
  }
  if (bh.blob_size != size || bh.blob_header_size != sizeof(SurfaceBlobHeader)) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: blob size mismatch");
  }
  if (bh.symbol_offset < sizeof(SurfaceBlobHeader) ||
      bh.slices_offset < sizeof(SurfaceBlobHeader)) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: payload overlaps header");
  }

  // Payload CRC (defense in depth alongside the whole-blob CRC above).
  if (crc32c(base + sizeof(SurfaceBlobHeader),
             static_cast<std::size_t>(size - sizeof(SurfaceBlobHeader))) != bh.payload_crc32c) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: payload checksum mismatch");
  }

  const auto param = static_cast<Parametrization>(bh.param);
  if (!serializable_param(param)) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: unsupported parametrization");
  }
  const std::size_t n = bh.n_slices;
  if (n == 0) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: zero slices");
  }
  const std::uint64_t elem = slice_elem_size(param);
  const std::uint64_t need = static_cast<std::uint64_t>(n) * elem;
  if (bh.slices_size < need) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: slice array too small");
  }
  if (bh.slices_offset > size || need > size - bh.slices_offset) {
    return Err(ErrorCode::ParseError, "SurfaceArchive::reconstruct: slices out of bounds");
  }

  auto surf = VolSurface::create(bh.uid, param, n);
  if (!surf) {
    return tl::unexpected<atx::core::Error>(std::move(surf).error());
  }
  VolSurface out = *std::move(surf);

  for (std::size_t i = 0; i < n; ++i) {
    const std::byte* sp =
        base + static_cast<std::size_t>(bh.slices_offset + static_cast<std::uint64_t>(i) * elem);
    if (param == Parametrization::Essvi) {
      EssviParams e;
      std::memcpy(&e, sp, sizeof e);
      auto st = out.set_slice_essvi(i, e);
      if (!st) {
        return tl::unexpected<atx::core::Error>(std::move(st).error());
      }
    } else {
      SviParams v;
      std::memcpy(&v, sp, sizeof v);
      auto st = out.set_slice_svi(i, v);
      if (!st) {
        return tl::unexpected<atx::core::Error>(std::move(st).error());
      }
    }
  }
  out.set_fit_ts_ns(bh.fit_ts_ns);
  VolSurface::Diagnostics diag;
  diag.rmse_vol = bh.rmse_vol;
  diag.max_residual_vol = bh.max_residual_vol;
  diag.n_quotes_used = bh.n_quotes_used;
  diag.n_quotes_dropped = bh.n_quotes_dropped;
  out.set_diagnostics(diag);

  return Ok(std::move(out));
}

Result<VolSurface> SurfaceArchive::map_symbol(std::string_view symbol) const {
  const ArchiveIndexSlot* s = find_slot(symbol);
  if (s == nullptr) {
    return Err(ErrorCode::NotFound, "SurfaceArchive::map_symbol: symbol not present");
  }
  return reconstruct(s->surface_offset, s->surface_size, s->surface_crc32c);
}

Result<std::vector<VolSurface>> SurfaceArchive::map_all() const {
  std::vector<VolSurface> out;
  out.reserve(directory_.size());
  const std::uint64_t mask =
      lookup_.empty() ? 0ull : static_cast<std::uint64_t>(lookup_.size()) - 1ull;
  for (const ArchiveDirEntry& de : directory_) {
    // Cross-reference the owning slot for the canonical whole-blob CRC.
    std::uint32_t expected = 0;
    bool found = false;
    std::uint64_t i = de.symbol_hash & mask;
    for (std::size_t step = 0; step < lookup_.size(); ++step) {
      const ArchiveIndexSlot& s = lookup_[static_cast<std::size_t>(i)];
      if (s.flags == kArchiveSlotEmpty) {
        break;
      }
      if (s.symbol_hash == de.symbol_hash && s.symbol_len == de.symbol_len &&
          std::memcmp(s.symbol, de.symbol, de.symbol_len) == 0) {
        expected = s.surface_crc32c;
        found = true;
        break;
      }
      i = (i + 1ull) & mask;
    }
    if (!found) {
      return Err(ErrorCode::ParseError, "SurfaceArchive::map_all: directory/lookup mismatch");
    }
    auto res = reconstruct(de.surface_offset, de.surface_size, expected);
    if (!res) {
      return tl::unexpected<atx::core::Error>(std::move(res).error());
    }
    out.push_back(*std::move(res));
  }
  return Ok(std::move(out));
}

Result<std::size_t> SurfaceArchive::map_all_into(std::span<std::optional<VolSurface>> out) const {
  if (out.size() < directory_.size()) {
    return Err(ErrorCode::OutOfRange, "SurfaceArchive::map_all_into: output too small");
  }
  auto all = map_all();
  if (!all) {
    return tl::unexpected<atx::core::Error>(std::move(all).error());
  }
  std::vector<VolSurface> surfaces = *std::move(all);
  for (std::size_t i = 0; i < surfaces.size(); ++i) {
    out[i].emplace(std::move(surfaces[i]));
  }
  return Ok(surfaces.size());
}

}  // namespace atx::vol
