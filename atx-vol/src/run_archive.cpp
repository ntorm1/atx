// RunArchive (ATXRUN01) — writer + reader for the binary backtest result
// container (run_archive.hpp / run_archive_schema.hpp).
//
// The writer mirrors `write_surface_archive_v2` (surface_archive.cpp): a
// two-pass plan-then-fill over one contiguous buffer — pass 1 sizes every
// section (header + column descriptors + 8-B-aligned typed arrays + dict/label
// aux tables), pass 2 memcpys the bytes and stamps the layered CRC-32Cs
// (header CRC with its own field zeroed; metadata CRC over the sorted section
// directory; per-section payload CRC with its own field zeroed, mirrored into
// the directory descriptor so any payload rewrite changes the archive
// identity — the F6 trick).

#include "atx/vol/run_archive.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/detail/archive_util.hpp" // crc32c, crc32c_update, align_up

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

using detail::align_up;
using detail::crc32c;

namespace {

// Fixed on-disk name capacities (the char[] widths in the Task 2 structs).
constexpr std::size_t kRaSectionNameMax = sizeof(RaSectionDescriptor{}.name); // 32
constexpr std::size_t kRaColumnNameMax = sizeof(RaColumnDescriptor{}.name);   // 40
constexpr std::size_t kRaUnitMax = sizeof(RaColumnDescriptor{}.unit);         // 16

[[nodiscard]] std::int64_t wall_clock_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

[[nodiscard]] const RaSection *registry_section(std::string_view name) noexcept {
  for (const RaSection &section : ra_sections()) {
    if (section.name == name) {
      return &section;
    }
  }
  return nullptr;
}

[[nodiscard]] const RaColumn *registry_column(const RaSection &section,
                                              std::string_view name) noexcept {
  for (const RaColumn &column : section.columns) {
    if (column.name == name) {
      return &column;
    }
  }
  return nullptr;
}

// CRC-32C over the whole 256-B header with its own crc field zeroed.
[[nodiscard]] std::uint32_t ra_header_crc(RunArchiveHeader h) noexcept {
  h.header_crc32c = 0;
  std::array<std::byte, sizeof(RunArchiveHeader)> raw{};
  std::memcpy(raw.data(), &h, sizeof h);
  return crc32c(raw.data(), raw.size());
}

// Number of staged values in the span that `dtype` selects.
[[nodiscard]] std::size_t column_length(const RaColumnData &c) noexcept {
  switch (c.dtype) {
  case RaDType::F64:
    return c.f64.size();
  case RaDType::I64:
    return c.i64.size();
  case RaDType::U32:
  case RaDType::DictStr:
    return c.u32.size();
  case RaDType::U8Enum:
    return c.u8.size();
  }
  return 0;
}

[[nodiscard]] constexpr bool dtype_has_aux(RaDType t) noexcept {
  return t == RaDType::DictStr || t == RaDType::U8Enum;
}

// ── Writer planning ──────────────────────────────────────────────────────────

struct ColPlan {
  const std::string *name{nullptr};
  std::string_view unit{};
  const RaColumnData *src{nullptr};
  std::uint64_t data_off{0}; // section-relative
  std::uint64_t data_size{0};
  std::uint64_t aux_off{0}; // 0 if no dict/label table
  std::uint64_t aux_size{0};
  std::uint32_t aux_count{0};
};

struct SecPlan {
  const RaSectionData *src{nullptr};
  std::uint64_t file_offset{0};
  std::uint64_t section_size{0};
  std::vector<ColPlan> cols{};
};

} // namespace

Result<std::vector<std::byte>> write_run_archive(std::span<const RaSectionData> sections,
                                                 std::int64_t created_ts_ns,
                                                 std::uint64_t run_identity_hash) {
  if (sections.empty()) {
    return Err(ErrorCode::InvalidArgument, "write_run_archive: no sections");
  }
  if (sections.size() > 0xFFFFFFFFull) {
    return Err(ErrorCode::InvalidArgument, "write_run_archive: too many sections");
  }

  // Deterministic directory order: sorted by name; a duplicate is an error.
  std::vector<std::size_t> order(sections.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    return sections[a].name < sections[b].name;
  });
  for (std::size_t i = 1; i < order.size(); ++i) {
    if (sections[order[i - 1]].name == sections[order[i]].name) {
      return Err(ErrorCode::AlreadyExists, "write_run_archive: duplicate section name");
    }
  }

  // 1. Plan + validate (section geometry: header → descriptors → 8-B-aligned
  //    column arrays, each dict/enum column followed by its aux table).
  std::vector<SecPlan> plans;
  plans.reserve(sections.size());
  for (const std::size_t idx : order) {
    const RaSectionData &sd = sections[idx];
    if (sd.name.empty() || sd.name.size() > kRaSectionNameMax) {
      return Err(ErrorCode::InvalidArgument, "write_run_archive: bad section name length");
    }
    if (static_cast<std::uint8_t>(sd.kind) > static_cast<std::uint8_t>(RaSectionKind::SubTable)) {
      return Err(ErrorCode::InvalidArgument, "write_run_archive: invalid section kind");
    }
    if (sd.columns.empty()) {
      return Err(ErrorCode::InvalidArgument, "write_run_archive: section has no columns");
    }
    if (sd.columns.size() > 0xFFFFFFFFull) {
      return Err(ErrorCode::InvalidArgument, "write_run_archive: too many columns");
    }
    if (sd.n_rows > (1ull << 48)) {
      return Err(ErrorCode::InvalidArgument, "write_run_archive: implausible row count");
    }
    const RaSection *reg = registry_section(sd.name);
    if (reg != nullptr && reg->kind != sd.kind) {
      return Err(ErrorCode::InvalidArgument,
                 "write_run_archive: section kind disagrees with the registry");
    }

    SecPlan plan;
    plan.src = &sd;
    plan.cols.reserve(sd.columns.size());
    std::uint64_t cursor =
        sizeof(RaSectionHeader) + sd.columns.size() * sizeof(RaColumnDescriptor);
    for (std::size_t ci = 0; ci < sd.columns.size(); ++ci) {
      const std::string &cname = sd.columns[ci].first;
      const RaColumnData &col = sd.columns[ci].second;
      if (cname.empty() || cname.size() > kRaColumnNameMax) {
        return Err(ErrorCode::InvalidArgument, "write_run_archive: bad column name length");
      }
      for (std::size_t cj = 0; cj < ci; ++cj) {
        if (sd.columns[cj].first == cname) {
          return Err(ErrorCode::AlreadyExists, "write_run_archive: duplicate column name");
        }
      }
      if (static_cast<std::uint8_t>(col.dtype) > static_cast<std::uint8_t>(RaDType::DictStr)) {
        return Err(ErrorCode::InvalidArgument, "write_run_archive: invalid column dtype");
      }
      if (column_length(col) != sd.n_rows) {
        return Err(ErrorCode::InvalidArgument,
                   "write_run_archive: column length disagrees with n_rows");
      }

      ColPlan cp;
      cp.name = &cname;
      cp.src = &col;
      // Registry-known (section, column) pairs must not drift; unknown columns
      // (dynamically appended per-signal series) pass through with unit "".
      if (reg != nullptr) {
        if (const RaColumn *rc = registry_column(*reg, cname); rc != nullptr) {
          if (rc->dtype != col.dtype) {
            return Err(ErrorCode::InvalidArgument,
                       "write_run_archive: column dtype disagrees with the registry");
          }
          cp.unit = rc->unit;
        }
      }
      if (cp.unit.size() > kRaUnitMax) {
        return Err(ErrorCode::InvalidArgument, "write_run_archive: unit tag too long");
      }

      cp.data_size = sd.n_rows * ra_dtype_size(col.dtype);
      cp.data_off = align_up(cursor, kRaColumnAlign);
      cursor = cp.data_off + cp.data_size;
      if (dtype_has_aux(col.dtype)) {
        if (col.strings.size() >= 0xFFFFFFFFull) {
          return Err(ErrorCode::InvalidArgument, "write_run_archive: string table too large");
        }
        cp.aux_count = static_cast<std::uint32_t>(col.strings.size());
        std::uint64_t blob_bytes = 0;
        for (const std::string &s : col.strings) {
          blob_bytes += s.size();
        }
        // Aux entry offsets are u32, so the concatenated bytes must fit one.
        if (blob_bytes > 0xFFFFFFFFull) {
          return Err(ErrorCode::InvalidArgument, "write_run_archive: string table exceeds 4 GiB");
        }
        if (col.dtype == RaDType::DictStr) {
          for (const std::uint32_t code : col.u32) {
            if (code >= cp.aux_count) {
              return Err(ErrorCode::InvalidArgument, "write_run_archive: dict code out of range");
            }
          }
        } else {
          for (const std::uint8_t code : col.u8) {
            if (code >= cp.aux_count) {
              return Err(ErrorCode::InvalidArgument, "write_run_archive: enum code out of range");
            }
          }
        }
        cp.aux_size = (static_cast<std::uint64_t>(cp.aux_count) + 1ull) * 4ull + blob_bytes;
        cp.aux_off = align_up(cursor, kRaColumnAlign);
        cursor = cp.aux_off + cp.aux_size;
      }
      plan.cols.push_back(cp);
    }
    plan.section_size = cursor;
    plans.push_back(std::move(plan));
  }

  // 2. File geometry: header → directory → 64-B-aligned sections.
  const std::uint64_t dir_offset = sizeof(RunArchiveHeader);
  const std::uint64_t dir_bytes =
      static_cast<std::uint64_t>(plans.size()) * sizeof(RaSectionDescriptor);
  const std::uint64_t data_offset = align_up(dir_offset + dir_bytes, kRaSectionAlign);
  std::uint64_t cursor = data_offset;
  for (SecPlan &plan : plans) {
    plan.file_offset = align_up(cursor, kRaSectionAlign);
    cursor = plan.file_offset + plan.section_size;
  }
  const std::uint64_t file_size = cursor;

  // 3. Materialize (buffer is zero-initialized: descriptor tails, alignment
  //    padding, and every reserved byte stay zero).
  std::vector<std::byte> buffer(static_cast<std::size_t>(file_size));
  std::vector<RaSectionDescriptor> directory(plans.size());

  for (std::size_t si = 0; si < plans.size(); ++si) {
    const SecPlan &plan = plans[si];
    const RaSectionData &sd = *plan.src;
    std::byte *base = buffer.data() + static_cast<std::size_t>(plan.file_offset);

    RaSectionHeader sh{};
    std::memcpy(sh.magic, kRaSectionMagic, 8);
    sh.section_size = plan.section_size;
    sh.n_rows = sd.n_rows;
    sh.n_cols = static_cast<std::uint32_t>(sd.columns.size());
    sh.col_desc_offset = static_cast<std::uint32_t>(sizeof(RaSectionHeader));
    sh.data_offset = static_cast<std::uint32_t>(plan.cols.front().data_off);
    sh.kind = sd.kind;
    std::memcpy(base, &sh, sizeof sh);

    for (std::size_t ci = 0; ci < plan.cols.size(); ++ci) {
      const ColPlan &cp = plan.cols[ci];
      const RaColumnData &col = *cp.src;

      RaColumnDescriptor cd{};
      cd.data_offset = cp.data_off;
      cd.data_size = cp.data_size;
      cd.aux_offset = cp.aux_off;
      cd.aux_size = cp.aux_size;
      cd.aux_count = cp.aux_count;
      cd.dtype = col.dtype;
      cd.name_len = static_cast<std::uint16_t>(cp.name->size());
      std::memcpy(cd.name, cp.name->data(), cp.name->size());
      if (!cp.unit.empty()) {
        std::memcpy(cd.unit, cp.unit.data(), cp.unit.size());
      }
      std::memcpy(base + sizeof(RaSectionHeader) + ci * sizeof(RaColumnDescriptor), &cd,
                  sizeof cd);

      if (cp.data_size > 0) {
        std::byte *dp = base + cp.data_off;
        switch (col.dtype) {
        case RaDType::F64:
          std::memcpy(dp, col.f64.data(), static_cast<std::size_t>(cp.data_size));
          break;
        case RaDType::I64:
          std::memcpy(dp, col.i64.data(), static_cast<std::size_t>(cp.data_size));
          break;
        case RaDType::U32:
        case RaDType::DictStr:
          std::memcpy(dp, col.u32.data(), static_cast<std::size_t>(cp.data_size));
          break;
        case RaDType::U8Enum:
          std::memcpy(dp, col.u8.data(), static_cast<std::size_t>(cp.data_size));
          break;
        }
      }
      if (cp.aux_off != 0) {
        // Aux table: u32 offsets[aux_count + 1] (offsets[0] = 0, ascending,
        // offsets[aux_count] = blob size) followed by the concatenated bytes.
        std::byte *ap = base + cp.aux_off;
        std::uint32_t running = 0;
        for (std::size_t k = 0; k < cp.aux_count; ++k) {
          std::memcpy(ap + 4ull * k, &running, 4);
          running += static_cast<std::uint32_t>(col.strings[k].size());
        }
        std::memcpy(ap + 4ull * cp.aux_count, &running, 4);
        std::byte *sp = ap + 4ull * (static_cast<std::uint64_t>(cp.aux_count) + 1ull);
        for (std::size_t k = 0; k < cp.aux_count; ++k) {
          if (!col.strings[k].empty()) {
            std::memcpy(sp, col.strings[k].data(), col.strings[k].size());
            sp += col.strings[k].size();
          }
        }
      }
    }

    // Payload CRC (the field is still zero in the buffer here) — stamped into
    // the section header AND mirrored into the directory descriptor so
    // metadata_crc32c is sensitive to any payload rewrite (F6).
    const std::uint32_t crc = crc32c(base, static_cast<std::size_t>(plan.section_size));
    std::memcpy(base + offsetof(RaSectionHeader, payload_crc32c), &crc, sizeof crc);

    RaSectionDescriptor &de = directory[si];
    de.section_offset = plan.file_offset;
    de.section_size = plan.section_size;
    de.n_rows = sd.n_rows;
    de.n_cols = static_cast<std::uint32_t>(sd.columns.size());
    de.col_desc_offset = static_cast<std::uint32_t>(sizeof(RaSectionHeader));
    de.payload_crc32c = crc;
    de.kind = sd.kind;
    std::memcpy(de.name, sd.name.data(), sd.name.size());
  }

  std::memcpy(buffer.data() + dir_offset, directory.data(), static_cast<std::size_t>(dir_bytes));
  const std::uint32_t meta =
      crc32c(buffer.data() + dir_offset, static_cast<std::size_t>(dir_bytes));

  // 4. Header.
  RunArchiveHeader hdr{};
  std::memcpy(hdr.magic, kRaMagic, 8);
  hdr.file_size = file_size;
  hdr.created_ts_ns = created_ts_ns != 0 ? static_cast<std::uint64_t>(created_ts_ns)
                                         : static_cast<std::uint64_t>(wall_clock_ns());
  hdr.schema_hash = ra_schema_hash();
  hdr.writer_version_hash = 0;
  hdr.run_identity_hash = run_identity_hash;
  hdr.section_dir_offset = dir_offset;
  hdr.data_offset = data_offset;
  hdr.section_count = static_cast<std::uint32_t>(plans.size());
  hdr.metadata_crc32c = meta;
  hdr.flags = 0;
  hdr.major = kRaMajor;
  hdr.minor = kRaMinor;
  hdr.header_size = static_cast<std::uint16_t>(sizeof(RunArchiveHeader));
  hdr.endian = 1;
  hdr.pointer_bits = 64;
  hdr.header_crc32c = ra_header_crc(hdr);
  std::memcpy(buffer.data(), &hdr, sizeof hdr);

  return Ok(std::move(buffer));
}

Status write_run_archive_file(std::string_view path, std::span<const RaSectionData> sections,
                              std::int64_t created_ts_ns, std::uint64_t run_identity_hash) {
  auto built = write_run_archive(sections, created_ts_ns, run_identity_hash);
  if (!built) {
    return tl::unexpected<atx::core::Error>(std::move(built).error());
  }
  const std::vector<std::byte> &buffer = *built;
  const std::filesystem::path dst{std::string(path)};
  std::filesystem::path tmp = dst;
  tmp += ".tmp";
  {
    std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
    if (!os) {
      return Err(ErrorCode::IoError, "write_run_archive_file: cannot open temp file");
    }
    os.write(reinterpret_cast<const char *>(buffer.data()),
             static_cast<std::streamsize>(buffer.size()));
    os.flush();
    if (!os) {
      std::error_code ec;
      std::filesystem::remove(tmp, ec);
      return Err(ErrorCode::IoError, "write_run_archive_file: write failed");
    }
  }
  // Atomic publish: remove any existing destination first (Windows rename does
  // not replace) then rename — mirrors the pending→rename pattern in
  // listed_dispersion_reconciliation.cpp.
  std::error_code ec;
  std::filesystem::remove(dst, ec);
  ec.clear();
  std::filesystem::rename(tmp, dst, ec);
  if (ec) {
    std::error_code ec2;
    std::filesystem::remove(tmp, ec2);
    return Err(ErrorCode::IoError, "write_run_archive_file: cannot publish file");
  }
  return Ok();
}

} // namespace atx::vol
