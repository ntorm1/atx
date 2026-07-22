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
//
// The reader mirrors `SurfaceArchiveV2::open_impl`'s bounds discipline: every
// offset/size is range-checked before any dereference, framing + header CRC +
// metadata CRC validate on open, and per-section payload CRC is LAZY
// (validate_section / validate_all only — never on the read path).

#include "atx/vol/run_archive.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/tsdb/mapping.hpp"            // tsdb::Mapping (read-only mmap seam)
#include "atx/vol/backtest.hpp"            // BacktestResult (Task 5 encoders)
#include "atx/vol/detail/archive_util.hpp" // crc32c, crc32c_update, align_up
#include "atx/vol/dispersion_workflow.hpp" // RunSpec (Task 5 meta encoder)
#include "atx/vol/listed_dispersion_reconciliation.hpp"
#include "atx/vol/listed_dispersion_schedule.hpp"
#include "atx/vol/surface_archive.hpp" // ArchiveContentIdentity (F6 identity)

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

using detail::align_up;
using detail::crc32c;
using detail::crc32c_update;

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

// CRC-32C over a whole section record with its own payload_crc32c field forced
// to 0 (the exact bytes the writer checksummed). Piecewise, no temp copy —
// mirrors record_crc_v2 in surface_archive.cpp.
[[nodiscard]] std::uint32_t ra_section_crc(const std::byte *base, std::uint64_t size) noexcept {
  constexpr std::size_t crc_off = offsetof(RaSectionHeader, payload_crc32c);
  const std::uint32_t zero = 0;
  std::uint32_t c = crc32c_update(0xFFFFFFFFu, base, crc_off);
  c = crc32c_update(c, reinterpret_cast<const std::byte *>(&zero), sizeof zero);
  c = crc32c_update(c, base + crc_off + sizeof(std::uint32_t),
                    static_cast<std::size_t>(size) - crc_off - sizeof(std::uint32_t));
  return c ^ 0xFFFFFFFFu;
}

// Effective section name in a directory descriptor: the char[32] is
// zero-padded and not NUL-terminated, so the name runs to the first NUL (or
// the full 32 bytes).
[[nodiscard]] std::string_view descriptor_name(const RaSectionDescriptor &d) noexcept {
  std::size_t len = 0;
  while (len < sizeof d.name && d.name[len] != '\0') {
    ++len;
  }
  return {d.name, len};
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

// ── Section encoders (Task 5): library type → staged RaSectionData ───────────
//
// RaColumnData is non-owning, so every array an encoder SYNTHESIZES (dict
// codes, string/label tables, flattened per-leg columns, NA-substituted
// doubles) lives in an EncoderArena parked on the returned section's
// type-erased `storage`. Columns that already exist as columnar vectors on the
// source object (the BacktestResult series) are spanned in place instead — the
// caller keeps that source alive across the write, per the RaColumnData rule.

namespace {

// Arena of finished column vectors. The OUTER vectors may reallocate as
// columns are appended, but spans point at the INNER vectors' heap blocks,
// which vector moves preserve — so a span taken over an arena-resident vector
// stays valid for the arena's lifetime.
struct EncoderArena {
  std::vector<std::vector<double>> f64;
  std::vector<std::vector<std::int64_t>> i64;
  std::vector<std::vector<std::uint32_t>> u32;
  std::vector<std::vector<std::uint8_t>> u8;
  std::vector<std::vector<std::string>> str;
};

RaColumnData arena_f64(EncoderArena &arena, std::vector<double> v) {
  arena.f64.push_back(std::move(v));
  return RaColumnData::of_f64(arena.f64.back());
}

RaColumnData arena_i64(EncoderArena &arena, std::vector<std::int64_t> v) {
  arena.i64.push_back(std::move(v));
  return RaColumnData::of_i64(arena.i64.back());
}

RaColumnData arena_u32(EncoderArena &arena, std::vector<std::uint32_t> v) {
  arena.u32.push_back(std::move(v));
  return RaColumnData::of_u32(arena.u32.back());
}

RaColumnData arena_u8enum(EncoderArena &arena, std::vector<std::uint8_t> codes,
                          std::vector<std::string> labels) {
  arena.u8.push_back(std::move(codes));
  arena.str.push_back(std::move(labels));
  return RaColumnData::of_u8enum(arena.u8.back(), arena.str.back());
}

RaColumnData arena_dict(EncoderArena &arena, std::vector<std::uint32_t> codes,
                        std::vector<std::string> table) {
  arena.u32.push_back(std::move(codes));
  arena.str.push_back(std::move(table));
  return RaColumnData::of_dict(arena.u32.back(), arena.str.back());
}

// Deterministic dict encoder: table entries in first-appearance order, so
// identical input sequences always yield identical codes + table bytes (the
// committed Python fixture depends on byte-stable output).
class DictBuilder {
public:
  void reserve(std::size_t n) { codes_.reserve(n); }
  void add(const std::string &value) {
    const auto [it, inserted] =
        index_.try_emplace(value, static_cast<std::uint32_t>(table_.size()));
    if (inserted) {
      table_.push_back(value);
    }
    codes_.push_back(it->second);
  }
  [[nodiscard]] RaColumnData finish(EncoderArena &arena) {
    index_.clear();
    return arena_dict(arena, std::move(codes_), std::move(table_));
  }

private:
  std::vector<std::string> table_;
  std::vector<std::uint32_t> codes_;
  std::unordered_map<std::string, std::uint32_t> index_;
};

// Enum label vocabularies. Codes are the C++ enum values, so labels[code] ==
// the exact token the mirrored TSV writer emits ('C'/'P', '0'/'1', or
// to_string(...) for role/status).
std::vector<std::string> bool_labels() { return {"0", "1"}; }
std::vector<std::string> side_labels() { return {"C", "P"}; }
std::vector<std::string> role_labels() { return {"Entry", "Held"}; }
std::vector<std::string> status_labels() {
  return {"Ok", "NoRawQuote", "CrossedQuote", "NoSurface", "PricingError"};
}

// "NA" in the mirrored TSV writers rides as quiet NaN in the pinned F64 dtype.
constexpr double kRaNa = std::numeric_limits<double>::quiet_NaN();

// Meta values are strings; doubles use %.17g (bit-exact round-trip decimal,
// per the registry's meta contract).
std::string format_meta_double(double v) {
  char buf[64];
  const int len = std::snprintf(buf, sizeof buf, "%.17g", v);
  return std::string(buf, static_cast<std::size_t>(len > 0 ? len : 0));
}

} // namespace

RaSectionData encode_backtest_section(std::string name, const BacktestResult &r) {
  auto arena = std::make_shared<EncoderArena>();
  RaSectionData sec;
  sec.name = std::move(name);
  sec.kind = RaSectionKind::TimeSeries;
  sec.n_rows = r.size();
  sec.columns.reserve(2 + 25 + r.signals.size());

  DictBuilder dates;
  dates.reserve(r.date.size());
  for (const std::string &d : r.date) {
    dates.add(d);
  }
  sec.columns.emplace_back("date", dates.finish(*arena));
  sec.columns.emplace_back("ts_ns", RaColumnData::of_i64(r.ts_ns));

  // EXACTLY the append_backtest_series_tsv member order (tearsheet.cpp), which
  // is the kBacktestCols registry order — value-equality with the TSV is spans
  // over the very vectors that writer serializes.
  const std::pair<const char *, const std::vector<double> *> dbl_cols[] = {
      {"pnl_total", &r.pnl_total},
      {"pnl_delta", &r.pnl_delta},
      {"pnl_gamma", &r.pnl_gamma},
      {"pnl_vega", &r.pnl_vega},
      {"pnl_vanna", &r.pnl_vanna},
      {"pnl_volga", &r.pnl_volga},
      {"pnl_theta", &r.pnl_theta},
      {"pnl_rho", &r.pnl_rho},
      {"pnl_charm", &r.pnl_charm},
      {"pnl_unexplained", &r.pnl_unexplained},
      {"pnl_settlement", &r.pnl_settlement},
      {"pnl_shares", &r.pnl_shares},
      {"financing", &r.financing},
      {"cost", &r.cost},
      {"nav", &r.nav},
      {"cash", &r.cash},
      {"gross_delta", &r.gross_delta},
      {"gross_gamma", &r.gross_gamma},
      {"gross_vega", &r.gross_vega},
      {"gross_theta", &r.gross_theta},
      {"turnover_notional", &r.turnover_notional},
      {"turnover_vega", &r.turnover_vega},
      {"n_open_lots", &r.n_open_lots},
      {"n_unpriced_lots", &r.n_unpriced_lots},
      {"n_unpriced_greeks", &r.n_unpriced_greeks},
  };
  for (const auto &[cname, col] : dbl_cols) {
    sec.columns.emplace_back(cname, RaColumnData::of_f64(*col));
  }
  // Per-signal series are appended dynamically after the registry columns,
  // exactly like the TSV writer appends them after the fixed header.
  for (const auto &sig : r.signals) {
    sec.columns.emplace_back(sig.first, RaColumnData::of_f64(sig.second));
  }
  sec.storage = std::move(arena);
  return sec;
}

RaSectionData
encode_reconciliation_section(const ListedDispersionReconciliation &reconciliation) {
  const std::vector<ListedReconciliationRow> &rows = reconciliation.rows;
  const std::size_t n = rows.size();
  auto arena = std::make_shared<EncoderArena>();
  RaSectionData sec;
  sec.name = "reconciliation";
  sec.kind = RaSectionKind::TimeSeries;
  sec.n_rows = n;

  DictBuilder date;
  date.reserve(n);
  std::vector<std::int64_t> valuation_ts(n);
  std::vector<std::uint32_t> held_cohort(n);
  std::vector<double> model_option_pnl(n);
  std::vector<double> quote_mid_pnl(n);
  std::vector<double> model_minus_quote_pnl(n);
  std::vector<double> model_nav(n);
  std::vector<double> quote_mid_nav(n);
  std::vector<double> quote_mid_coverage(n);
  std::vector<std::uint32_t> n_held_lots(n);
  std::vector<std::uint32_t> n_quote_mid_lots(n);
  for (std::size_t i = 0; i < n; ++i) {
    const ListedReconciliationRow &row = rows[i];
    date.add(row.date);
    valuation_ts[i] = row.valuation_ts_ns;
    held_cohort[i] = row.held_cohort;
    model_option_pnl[i] = row.model_option_pnl;
    quote_mid_pnl[i] = row.quote_mid_pnl;
    model_minus_quote_pnl[i] = row.model_minus_quote_pnl;
    model_nav[i] = row.model_nav;
    quote_mid_nav[i] = row.quote_mid_nav;
    quote_mid_coverage[i] = row.quote_mid_coverage;
    n_held_lots[i] = row.n_held_lots;
    n_quote_mid_lots[i] = row.n_quote_mid_lots;
  }

  sec.columns.reserve(11);
  sec.columns.emplace_back("date", date.finish(*arena));
  sec.columns.emplace_back("valuation_ts_ns", arena_i64(*arena, std::move(valuation_ts)));
  sec.columns.emplace_back("held_cohort", arena_u32(*arena, std::move(held_cohort)));
  sec.columns.emplace_back("model_option_pnl", arena_f64(*arena, std::move(model_option_pnl)));
  sec.columns.emplace_back("quote_mid_pnl", arena_f64(*arena, std::move(quote_mid_pnl)));
  sec.columns.emplace_back("model_minus_quote_pnl",
                           arena_f64(*arena, std::move(model_minus_quote_pnl)));
  sec.columns.emplace_back("model_nav", arena_f64(*arena, std::move(model_nav)));
  sec.columns.emplace_back("quote_mid_nav", arena_f64(*arena, std::move(quote_mid_nav)));
  sec.columns.emplace_back("quote_mid_coverage",
                           arena_f64(*arena, std::move(quote_mid_coverage)));
  sec.columns.emplace_back("n_held_lots", arena_u32(*arena, std::move(n_held_lots)));
  sec.columns.emplace_back("n_quote_mid_lots", arena_u32(*arena, std::move(n_quote_mid_lots)));
  sec.storage = std::move(arena);
  return sec;
}

RaSectionData encode_schedule_section(std::string name,
                                      const ListedDispersionSchedule &schedule) {
  std::size_t n = 0;
  for (const ListedScheduleRoll &roll : schedule.rolls) {
    n += roll.legs.size();
  }
  auto arena = std::make_shared<EncoderArena>();
  RaSectionData sec;
  sec.name = std::move(name);
  sec.kind = RaSectionKind::SubTable;
  sec.n_rows = n;

  DictBuilder roll_date;
  DictBuilder symbol;
  DictBuilder raw_symbol;
  roll_date.reserve(n);
  symbol.reserve(n);
  raw_symbol.reserve(n);
  std::vector<std::int64_t> valuation_ts(n);
  std::vector<std::uint32_t> cohort(n);
  std::vector<std::int64_t> expiry_ts(n);
  std::vector<double> gross_index_vega_target(n);
  std::vector<double> net_vega(n);
  std::vector<double> gross_vega(n);
  std::vector<std::uint32_t> n_names(n);
  std::vector<std::uint8_t> is_index(n);
  std::vector<std::uint32_t> uid(n);
  std::vector<std::uint32_t> instrument_id(n);
  std::vector<double> strike(n);
  std::vector<std::uint8_t> side(n);
  std::vector<double> quantity(n);
  std::vector<double> multiplier(n);
  std::vector<double> raw_bid(n);
  std::vector<double> raw_ask(n);
  std::vector<double> raw_mid(n);
  std::vector<double> model_mark(n);
  std::vector<double> delta_per_share(n);
  std::vector<double> vega_per_unit_vol(n);
  std::vector<double> vega_per_contract(n);
  std::vector<double> normalized_weight(n);
  std::vector<double> target_straddle_vega(n);
  std::vector<double> achieved_leg_vega(n);
  std::vector<std::int64_t> source_fingerprint(n);
  std::vector<std::int64_t> surface_fingerprint(n);

  std::size_t i = 0;
  for (const ListedScheduleRoll &roll : schedule.rolls) {
    for (const ListedScheduleLeg &leg : roll.legs) {
      // Roll fields repeat on every leg row — the flattened TSV shape.
      roll_date.add(roll.roll_date);
      valuation_ts[i] = roll.valuation_ts_ns;
      cohort[i] = roll.cohort;
      expiry_ts[i] = roll.expiry_ts_ns;
      gross_index_vega_target[i] = roll.gross_index_vega_target_per_vol_point;
      net_vega[i] = roll.net_vega_per_vol_point;
      gross_vega[i] = roll.gross_vega_per_vol_point;
      n_names[i] = roll.n_names;
      is_index[i] = leg.is_index ? std::uint8_t{1} : std::uint8_t{0};
      symbol.add(leg.symbol);
      uid[i] = leg.uid;
      instrument_id[i] = leg.instrument_id;
      raw_symbol.add(leg.raw_symbol);
      strike[i] = leg.strike;
      side[i] = static_cast<std::uint8_t>(leg.side);
      quantity[i] = leg.quantity;
      multiplier[i] = leg.multiplier;
      raw_bid[i] = leg.raw_bid;
      raw_ask[i] = leg.raw_ask;
      raw_mid[i] = leg.raw_mid;
      model_mark[i] = leg.model_mark;
      delta_per_share[i] = leg.delta_per_share;
      vega_per_unit_vol[i] = leg.vega_per_unit_vol;
      vega_per_contract[i] = leg.vega_per_contract_per_vol_point;
      normalized_weight[i] = leg.normalized_weight;
      target_straddle_vega[i] = leg.target_straddle_vega_per_vol_point;
      achieved_leg_vega[i] = leg.achieved_leg_vega_per_vol_point;
      // u64 fingerprints ride as I64 bit patterns (no U64 dtype by design).
      source_fingerprint[i] = static_cast<std::int64_t>(leg.source_fingerprint);
      surface_fingerprint[i] = static_cast<std::int64_t>(leg.surface_fingerprint);
      ++i;
    }
  }

  sec.columns.reserve(29);
  sec.columns.emplace_back("roll_date", roll_date.finish(*arena));
  sec.columns.emplace_back("valuation_ts_ns", arena_i64(*arena, std::move(valuation_ts)));
  sec.columns.emplace_back("cohort", arena_u32(*arena, std::move(cohort)));
  sec.columns.emplace_back("expiry_ts_ns", arena_i64(*arena, std::move(expiry_ts)));
  sec.columns.emplace_back("gross_index_vega_target",
                           arena_f64(*arena, std::move(gross_index_vega_target)));
  sec.columns.emplace_back("net_vega", arena_f64(*arena, std::move(net_vega)));
  sec.columns.emplace_back("gross_vega", arena_f64(*arena, std::move(gross_vega)));
  sec.columns.emplace_back("n_names", arena_u32(*arena, std::move(n_names)));
  sec.columns.emplace_back("is_index", arena_u8enum(*arena, std::move(is_index), bool_labels()));
  sec.columns.emplace_back("symbol", symbol.finish(*arena));
  sec.columns.emplace_back("uid", arena_u32(*arena, std::move(uid)));
  sec.columns.emplace_back("instrument_id", arena_u32(*arena, std::move(instrument_id)));
  sec.columns.emplace_back("raw_symbol", raw_symbol.finish(*arena));
  sec.columns.emplace_back("strike", arena_f64(*arena, std::move(strike)));
  sec.columns.emplace_back("side", arena_u8enum(*arena, std::move(side), side_labels()));
  sec.columns.emplace_back("quantity", arena_f64(*arena, std::move(quantity)));
  sec.columns.emplace_back("multiplier", arena_f64(*arena, std::move(multiplier)));
  sec.columns.emplace_back("raw_bid", arena_f64(*arena, std::move(raw_bid)));
  sec.columns.emplace_back("raw_ask", arena_f64(*arena, std::move(raw_ask)));
  sec.columns.emplace_back("raw_mid", arena_f64(*arena, std::move(raw_mid)));
  sec.columns.emplace_back("model_mark", arena_f64(*arena, std::move(model_mark)));
  sec.columns.emplace_back("delta_per_share", arena_f64(*arena, std::move(delta_per_share)));
  sec.columns.emplace_back("vega_per_unit_vol",
                           arena_f64(*arena, std::move(vega_per_unit_vol)));
  sec.columns.emplace_back("vega_per_contract_per_vol_point",
                           arena_f64(*arena, std::move(vega_per_contract)));
  sec.columns.emplace_back("normalized_weight",
                           arena_f64(*arena, std::move(normalized_weight)));
  sec.columns.emplace_back("target_straddle_vega",
                           arena_f64(*arena, std::move(target_straddle_vega)));
  sec.columns.emplace_back("achieved_leg_vega",
                           arena_f64(*arena, std::move(achieved_leg_vega)));
  sec.columns.emplace_back("source_fingerprint",
                           arena_i64(*arena, std::move(source_fingerprint)));
  sec.columns.emplace_back("surface_fingerprint",
                           arena_i64(*arena, std::move(surface_fingerprint)));
  sec.storage = std::move(arena);
  return sec;
}

RaSectionData
encode_contract_marks_section(const ListedDispersionReconciliation &reconciliation) {
  const std::vector<ListedContractMark> &marks = reconciliation.marks;
  const std::size_t n = marks.size();
  auto arena = std::make_shared<EncoderArena>();
  RaSectionData sec;
  sec.name = "contract_marks";
  sec.kind = RaSectionKind::SubTable;
  sec.n_rows = n;

  DictBuilder date;
  DictBuilder symbol;
  DictBuilder raw_symbol;
  date.reserve(n);
  symbol.reserve(n);
  raw_symbol.reserve(n);
  std::vector<std::int64_t> valuation_ts(n);
  std::vector<std::uint8_t> role(n);
  std::vector<std::uint32_t> cohort(n);
  std::vector<std::uint32_t> uid(n);
  std::vector<std::uint32_t> instrument_id(n);
  std::vector<std::int64_t> expiry_ts(n);
  std::vector<double> strike(n);
  std::vector<std::uint8_t> side(n);
  std::vector<double> quantity(n);
  std::vector<double> multiplier(n);
  std::vector<std::uint8_t> status(n);
  std::vector<double> raw_bid(n);
  std::vector<double> raw_ask(n);
  std::vector<double> raw_mid(n);
  std::vector<double> model_mark(n);
  std::vector<std::uint8_t> model_in_spread(n);
  for (std::size_t i = 0; i < n; ++i) {
    const ListedContractMark &m = marks[i];
    date.add(m.date);
    valuation_ts[i] = m.valuation_ts_ns;
    role[i] = static_cast<std::uint8_t>(m.role);
    cohort[i] = m.cohort;
    symbol.add(m.symbol);
    uid[i] = m.uid;
    instrument_id[i] = m.instrument_id;
    raw_symbol.add(m.raw_symbol);
    expiry_ts[i] = m.expiry_ts_ns;
    strike[i] = m.strike;
    side[i] = static_cast<std::uint8_t>(m.side);
    quantity[i] = m.quantity;
    multiplier[i] = m.multiplier;
    status[i] = static_cast<std::uint8_t>(m.status);
    // NA convention mirrors serialize_listed_contract_marks: raw quote fields
    // are meaningful only when status == Ok, the model mark only when a
    // surface priced (neither NoSurface nor PricingError). The registry pins
    // F64 for these, so "NA" rides as quiet NaN.
    const bool raw = m.status == ListedMarkStatus::Ok;
    const bool model =
        m.status != ListedMarkStatus::NoSurface && m.status != ListedMarkStatus::PricingError;
    raw_bid[i] = raw ? m.raw_bid : kRaNa;
    raw_ask[i] = raw ? m.raw_ask : kRaNa;
    raw_mid[i] = raw ? m.raw_mid : kRaNa;
    model_mark[i] = model ? m.model_mark : kRaNa;
    model_in_spread[i] = m.model_in_spread ? std::uint8_t{1} : std::uint8_t{0};
  }

  sec.columns.reserve(19);
  sec.columns.emplace_back("date", date.finish(*arena));
  sec.columns.emplace_back("valuation_ts_ns", arena_i64(*arena, std::move(valuation_ts)));
  sec.columns.emplace_back("role", arena_u8enum(*arena, std::move(role), role_labels()));
  sec.columns.emplace_back("cohort", arena_u32(*arena, std::move(cohort)));
  sec.columns.emplace_back("symbol", symbol.finish(*arena));
  sec.columns.emplace_back("uid", arena_u32(*arena, std::move(uid)));
  sec.columns.emplace_back("instrument_id", arena_u32(*arena, std::move(instrument_id)));
  sec.columns.emplace_back("raw_symbol", raw_symbol.finish(*arena));
  sec.columns.emplace_back("expiry_ts_ns", arena_i64(*arena, std::move(expiry_ts)));
  sec.columns.emplace_back("strike", arena_f64(*arena, std::move(strike)));
  sec.columns.emplace_back("side", arena_u8enum(*arena, std::move(side), side_labels()));
  sec.columns.emplace_back("quantity", arena_f64(*arena, std::move(quantity)));
  sec.columns.emplace_back("multiplier", arena_f64(*arena, std::move(multiplier)));
  sec.columns.emplace_back("status", arena_u8enum(*arena, std::move(status), status_labels()));
  sec.columns.emplace_back("raw_bid", arena_f64(*arena, std::move(raw_bid)));
  sec.columns.emplace_back("raw_ask", arena_f64(*arena, std::move(raw_ask)));
  sec.columns.emplace_back("raw_mid", arena_f64(*arena, std::move(raw_mid)));
  sec.columns.emplace_back("model_mark", arena_f64(*arena, std::move(model_mark)));
  sec.columns.emplace_back("model_in_spread",
                           arena_u8enum(*arena, std::move(model_in_spread), bool_labels()));
  sec.storage = std::move(arena);
  return sec;
}

RaSectionData encode_meta_section(const RunSpec &spec,
                                  std::span<const std::pair<std::string, std::string>> extra) {
  // The resolved-spec echo mirrors write_resolved_spec's key vocabulary and
  // order (dispersion_workflow.cpp), including its conditional keys; doubles
  // use %.17g per the registry's meta contract (write_resolved_spec's
  // default iostream precision would truncate).
  std::vector<std::pair<std::string, std::string>> pairs;
  pairs.reserve(21 + extra.size());
  pairs.emplace_back("label", spec.label);
  pairs.emplace_back("date_lo", spec.date_lo);
  pairs.emplace_back("date_hi", spec.date_hi);
  pairs.emplace_back("snapshot_suffix", spec.snapshot_suffix);
  pairs.emplace_back("opra_root", spec.opra_root.string());
  pairs.emplace_back("path_template", spec.path_template);
  pairs.emplace_back("universe_schedule", spec.universe_path.string());
  if (!spec.definitions_path.empty()) {
    pairs.emplace_back("definitions", spec.definitions_path.string());
  }
  if (!spec.occ_ess_root.empty()) {
    pairs.emplace_back("occ_ess_root", spec.occ_ess_root.string());
  }
  pairs.emplace_back("flat_rate", format_meta_double(spec.flat_rate));
  pairs.emplace_back("min_names", std::to_string(spec.min_names));
  pairs.emplace_back("min_weight_coverage", format_meta_double(spec.min_weight_coverage));
  pairs.emplace_back("target_dte_days", format_meta_double(spec.target_dte_days));
  pairs.emplace_back("min_dte_days", format_meta_double(spec.min_dte_days));
  pairs.emplace_back("max_dte_days", format_meta_double(spec.max_dte_days));
  pairs.emplace_back("roll_dte_days", format_meta_double(spec.roll_dte_days));
  pairs.emplace_back("gross_index_vega", format_meta_double(spec.gross_index_vega));
  pairs.emplace_back("delta_band", format_meta_double(spec.delta_band));
  pairs.emplace_back("fit_workers", std::to_string(spec.fit_workers));
  pairs.emplace_back("core_mode", spec.core_mode ? "1" : "0");
  for (const auto &kv : extra) {
    pairs.push_back(kv);
  }

  auto arena = std::make_shared<EncoderArena>();
  RaSectionData sec;
  sec.name = "meta";
  sec.kind = RaSectionKind::ScalarKV;
  sec.n_rows = pairs.size();
  DictBuilder keys;
  DictBuilder values;
  keys.reserve(pairs.size());
  values.reserve(pairs.size());
  for (const auto &[k, v] : pairs) {
    keys.add(k);
    values.add(v);
  }
  sec.columns.reserve(2);
  sec.columns.emplace_back("key", keys.finish(*arena));
  sec.columns.emplace_back("value", values.finish(*arena));
  sec.storage = std::move(arena);
  return sec;
}

// ── Reader ───────────────────────────────────────────────────────────────────

Result<RunArchive> RunArchive::open_impl(std::span<const std::byte> bytes,
                                         std::shared_ptr<const void> owner) {
  if (bytes.size() < sizeof(RunArchiveHeader)) {
    return Err(ErrorCode::ParseError, "RunArchive::open: shorter than header");
  }
  RunArchiveHeader h;
  std::memcpy(&h, bytes.data(), sizeof h);
  if (std::memcmp(h.magic, kRaMagic, 8) != 0) {
    return Err(ErrorCode::ParseError, "RunArchive::open: bad magic");
  }
  if (h.major != kRaMajor) {
    return Err(ErrorCode::ParseError, "RunArchive::open: unsupported major version");
  }
  if (h.minor > kRaMinor) {
    return Err(ErrorCode::ParseError, "RunArchive::open: unsupported minor version");
  }
  if (h.endian != 1) {
    return Err(ErrorCode::ParseError, "RunArchive::open: non-little-endian archive");
  }
  if (h.pointer_bits != 64) {
    return Err(ErrorCode::ParseError, "RunArchive::open: unsupported pointer width");
  }
  if (h.header_size != sizeof(RunArchiveHeader)) {
    return Err(ErrorCode::ParseError, "RunArchive::open: header size mismatch");
  }
  if (h.schema_hash != ra_schema_hash()) {
    return Err(ErrorCode::ParseError, "RunArchive::open: schema hash mismatch");
  }
  if (h.file_size != bytes.size()) {
    return Err(ErrorCode::ParseError, "RunArchive::open: file size mismatch");
  }
  const std::uint64_t dir_bytes =
      static_cast<std::uint64_t>(h.section_count) * sizeof(RaSectionDescriptor);
  if (h.section_dir_offset < sizeof(RunArchiveHeader)) {
    return Err(ErrorCode::ParseError, "RunArchive::open: directory overlaps header");
  }
  if (h.section_dir_offset > h.file_size || dir_bytes > h.file_size - h.section_dir_offset) {
    return Err(ErrorCode::ParseError, "RunArchive::open: directory out of bounds");
  }
  if (h.section_dir_offset + dir_bytes > h.data_offset) {
    return Err(ErrorCode::ParseError, "RunArchive::open: directory overlaps data");
  }
  if (h.data_offset > h.file_size) {
    return Err(ErrorCode::ParseError, "RunArchive::open: data offset out of bounds");
  }
  if (ra_header_crc(h) != h.header_crc32c) {
    return Err(ErrorCode::ParseError, "RunArchive::open: header checksum mismatch");
  }
  if (crc32c(bytes.data() + h.section_dir_offset, static_cast<std::size_t>(dir_bytes)) !=
      h.metadata_crc32c) {
    return Err(ErrorCode::ParseError, "RunArchive::open: metadata checksum mismatch");
  }

  RunArchive a;
  a.bytes_ = bytes;
  a.owner_ = std::move(owner);
  a.header_ = h;
  a.directory_.resize(h.section_count);
  if (dir_bytes > 0) {
    std::memcpy(a.directory_.data(), bytes.data() + h.section_dir_offset,
                static_cast<std::size_t>(dir_bytes));
  }
  for (const RaSectionDescriptor &de : a.directory_) {
    if (descriptor_name(de).empty()) {
      return Err(ErrorCode::ParseError, "RunArchive::open: empty section name");
    }
    if (static_cast<std::uint8_t>(de.kind) > static_cast<std::uint8_t>(RaSectionKind::SubTable)) {
      return Err(ErrorCode::ParseError, "RunArchive::open: invalid section kind");
    }
    if (de.section_offset < h.data_offset) {
      return Err(ErrorCode::ParseError, "RunArchive::open: section precedes data");
    }
    if (de.section_offset > h.file_size || de.section_size > h.file_size - de.section_offset) {
      return Err(ErrorCode::ParseError, "RunArchive::open: section out of bounds");
    }
    if (de.section_size < sizeof(RaSectionHeader)) {
      return Err(ErrorCode::ParseError, "RunArchive::open: section smaller than header");
    }
    // Section starts must be >= 8-B aligned in-file so typed column reads are
    // aligned relative to a >= 8-B backing base (mirrors the v2 §11.3
    // hardening). Sections are packed on kRaSectionAlign (>= 8), so any entry
    // not so aligned is corrupt.
    if ((de.section_offset % kRaColumnAlign) != 0u) {
      return Err(ErrorCode::ParseError, "RunArchive::open: section offset misaligned");
    }
  }
  return Ok(std::move(a));
}

Result<RunArchive> RunArchive::open(std::vector<std::byte> bytes) {
  auto owned = std::make_shared<std::vector<std::byte>>(std::move(bytes));
  std::span<const std::byte> span{owned->data(), owned->size()};
  return open_impl(span, std::static_pointer_cast<const void>(owned));
}

Result<RunArchive> RunArchive::open_borrowed(std::span<const std::byte> bytes,
                                             std::shared_ptr<const void> owner) {
  return open_impl(bytes, std::move(owner));
}

Result<RunArchive> RunArchive::open_file(std::string_view path) {
  const std::filesystem::path p{std::string(path)};
  std::error_code ec;
  if (!std::filesystem::exists(p, ec) || ec) {
    return Err(ErrorCode::NotFound, "RunArchive::open_file: file not found");
  }
  std::ifstream is(p, std::ios::binary | std::ios::ate);
  if (!is) {
    return Err(ErrorCode::IoError, "RunArchive::open_file: cannot open file");
  }
  const std::streamsize size = is.tellg();
  if (size < 0) {
    return Err(ErrorCode::IoError, "RunArchive::open_file: cannot size file");
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  is.seekg(0);
  is.read(reinterpret_cast<char *>(bytes.data()), size);
  if (is.gcount() != size) {
    return Err(ErrorCode::IoError, "RunArchive::open_file: short read");
  }
  return open(std::move(bytes));
}

Result<RunArchive> RunArchive::open_mapped(std::string_view path) {
  // Same mmap seam as SurfaceArchiveV2::open_mapped: map read-only and validate
  // through open_borrowed, so opening faults in only the metadata pages (header
  // + directory); section bytes fault in lazily via the OS page cache when a
  // view touches them. The Mapping is kept alive for the archive's whole
  // lifetime via the type-erased owner — every RaSectionView borrows into the
  // mapped pages and must not outlive the archive.
  auto mapping = atx::tsdb::Mapping::map_file_ro(std::string(path));
  if (!mapping) {
    return tl::unexpected<atx::core::Error>(std::move(mapping).error());
  }
  auto owner = std::make_shared<atx::tsdb::Mapping>(std::move(*mapping));
  const std::span<const std::byte> span{reinterpret_cast<const std::byte *>(owner->base()),
                                        static_cast<std::size_t>(owner->size())};
  return open_borrowed(span, std::static_pointer_cast<const void>(owner));
}

ArchiveContentIdentity RunArchive::identity() const noexcept {
  return ArchiveContentIdentity{header_.file_size, header_.created_ts_ns, header_.header_crc32c,
                                header_.metadata_crc32c};
}

const RaSectionDescriptor *RunArchive::find_descriptor(std::string_view name) const noexcept {
  for (const RaSectionDescriptor &de : directory_) {
    if (descriptor_name(de) == name) {
      return &de;
    }
  }
  return nullptr;
}

Result<RaSectionView> RunArchive::section(std::string_view name) const {
  const RaSectionDescriptor *de = find_descriptor(name);
  if (de == nullptr) {
    return Err(ErrorCode::NotFound, "RunArchive::section: section not present");
  }
  // de's extent was bounds-checked against the file at open().
  const std::byte *base = bytes_.data() + de->section_offset;
  RaSectionHeader sh;
  std::memcpy(&sh, base, sizeof sh);
  if (std::memcmp(sh.magic, kRaSectionMagic, 8) != 0) {
    return Err(ErrorCode::ParseError, "RunArchive::section: bad section magic");
  }
  if (sh.section_size != de->section_size || sh.n_rows != de->n_rows ||
      sh.n_cols != de->n_cols || sh.kind != de->kind ||
      sh.col_desc_offset != de->col_desc_offset) {
    return Err(ErrorCode::ParseError, "RunArchive::section: descriptor disagreement");
  }
  if (sh.n_cols == 0) {
    return Err(ErrorCode::ParseError, "RunArchive::section: section has no columns");
  }
  const std::uint64_t desc_bytes =
      static_cast<std::uint64_t>(sh.n_cols) * sizeof(RaColumnDescriptor);
  if (sh.col_desc_offset < sizeof(RaSectionHeader) || sh.col_desc_offset > sh.section_size ||
      desc_bytes > sh.section_size - sh.col_desc_offset) {
    return Err(ErrorCode::ParseError, "RunArchive::section: column descriptors out of bounds");
  }

  RaSectionView view;
  view.base_ = base;
  view.header_ = sh;
  view.name_ = std::string(name);
  view.cols_.resize(sh.n_cols);
  std::memcpy(view.cols_.data(), base + sh.col_desc_offset,
              static_cast<std::size_t>(desc_bytes));
  for (const RaColumnDescriptor &cd : view.cols_) {
    if (cd.name_len == 0 || cd.name_len > sizeof cd.name) {
      return Err(ErrorCode::ParseError, "RunArchive::section: column name length out of bounds");
    }
    if (static_cast<std::uint8_t>(cd.dtype) > static_cast<std::uint8_t>(RaDType::DictStr)) {
      return Err(ErrorCode::ParseError, "RunArchive::section: invalid column dtype");
    }
    if ((cd.data_offset % kRaColumnAlign) != 0u) {
      return Err(ErrorCode::ParseError, "RunArchive::section: column data misaligned");
    }
    if (cd.data_offset > sh.section_size || cd.data_size > sh.section_size - cd.data_offset) {
      return Err(ErrorCode::ParseError, "RunArchive::section: column data out of bounds");
    }
    if (cd.data_size != sh.n_rows * ra_dtype_size(cd.dtype)) {
      return Err(ErrorCode::ParseError, "RunArchive::section: column size disagrees with n_rows");
    }
    if (dtype_has_aux(cd.dtype)) {
      // Aux table framing: u32 offsets[aux_count + 1] then the string blob.
      if (cd.aux_offset == 0 || (cd.aux_offset % 4u) != 0u) {
        return Err(ErrorCode::ParseError, "RunArchive::section: aux table missing or misaligned");
      }
      if (cd.aux_offset > sh.section_size || cd.aux_size > sh.section_size - cd.aux_offset) {
        return Err(ErrorCode::ParseError, "RunArchive::section: aux table out of bounds");
      }
      const std::uint64_t offsets_bytes = (static_cast<std::uint64_t>(cd.aux_count) + 1ull) * 4ull;
      if (offsets_bytes > cd.aux_size) {
        return Err(ErrorCode::ParseError, "RunArchive::section: aux offsets out of bounds");
      }
      const std::uint64_t blob_bytes = cd.aux_size - offsets_bytes;
      const std::byte *offsets = base + cd.aux_offset;
      std::uint32_t prev = 0;
      std::memcpy(&prev, offsets, 4);
      if (prev != 0) {
        return Err(ErrorCode::ParseError, "RunArchive::section: aux table does not start at 0");
      }
      for (std::uint64_t k = 1; k <= cd.aux_count; ++k) {
        std::uint32_t cur = 0;
        std::memcpy(&cur, offsets + 4 * k, 4);
        if (cur < prev) {
          return Err(ErrorCode::ParseError, "RunArchive::section: aux offsets not monotone");
        }
        prev = cur;
      }
      if (prev != blob_bytes) {
        return Err(ErrorCode::ParseError, "RunArchive::section: aux blob size disagreement");
      }
      // Every code must index the table so view accessors are unchecked-safe.
      if (cd.dtype == RaDType::DictStr) {
        const auto *codes = reinterpret_cast<const std::uint32_t *>(base + cd.data_offset);
        for (std::uint64_t r = 0; r < sh.n_rows; ++r) {
          if (codes[r] >= cd.aux_count) {
            return Err(ErrorCode::ParseError, "RunArchive::section: dict code out of range");
          }
        }
      } else {
        const auto *codes = reinterpret_cast<const std::uint8_t *>(base + cd.data_offset);
        for (std::uint64_t r = 0; r < sh.n_rows; ++r) {
          if (codes[r] >= cd.aux_count) {
            return Err(ErrorCode::ParseError, "RunArchive::section: enum code out of range");
          }
        }
      }
    } else if (cd.aux_offset != 0 || cd.aux_size != 0 || cd.aux_count != 0) {
      return Err(ErrorCode::ParseError, "RunArchive::section: unexpected aux table");
    }
  }
  return Ok(std::move(view));
}

Status RunArchive::validate_section(std::string_view name) const {
  const RaSectionDescriptor *de = find_descriptor(name);
  if (de == nullptr) {
    return Err(ErrorCode::NotFound, "RunArchive::validate_section: section not present");
  }
  // Extent bounds-checked at open(); size >= sizeof(RaSectionHeader) too.
  const std::byte *base = bytes_.data() + de->section_offset;
  std::uint32_t stored = 0;
  std::memcpy(&stored, base + offsetof(RaSectionHeader, payload_crc32c), sizeof stored);
  if (stored != de->payload_crc32c) {
    return Err(ErrorCode::ParseError,
               "RunArchive::validate: section/directory checksum disagreement");
  }
  if (ra_section_crc(base, de->section_size) != stored) {
    return Err(ErrorCode::ParseError, "RunArchive::validate: section checksum mismatch");
  }
  return Ok();
}

Status RunArchive::validate_all() const {
  for (const RaSectionDescriptor &de : directory_) {
    const Status st = validate_section(descriptor_name(de));
    if (!st) {
      return st;
    }
  }
  return Ok();
}

// ── RaSectionView accessors ──────────────────────────────────────────────────

const RaColumnDescriptor *RaSectionView::find_col(std::string_view name,
                                                  RaDType dtype) const noexcept {
  for (const RaColumnDescriptor &cd : cols_) {
    if (cd.dtype == dtype && cd.name_len == name.size() &&
        std::memcmp(cd.name, name.data(), name.size()) == 0) {
      return &cd;
    }
  }
  return nullptr;
}

RaStringTable RaSectionView::string_table(const RaColumnDescriptor &cd) const noexcept {
  const std::byte *offsets = base_ + cd.aux_offset;
  const std::byte *blob = offsets + (static_cast<std::uint64_t>(cd.aux_count) + 1ull) * 4ull;
  return RaStringTable{offsets, blob, cd.aux_count};
}

std::span<const double> RaSectionView::f64_col(std::string_view name) const noexcept {
  const RaColumnDescriptor *cd = find_col(name, RaDType::F64);
  if (cd == nullptr) {
    return {};
  }
  return {reinterpret_cast<const double *>(base_ + cd->data_offset),
          static_cast<std::size_t>(header_.n_rows)};
}

std::span<const std::int64_t> RaSectionView::i64_col(std::string_view name) const noexcept {
  const RaColumnDescriptor *cd = find_col(name, RaDType::I64);
  if (cd == nullptr) {
    return {};
  }
  return {reinterpret_cast<const std::int64_t *>(base_ + cd->data_offset),
          static_cast<std::size_t>(header_.n_rows)};
}

std::span<const std::uint32_t> RaSectionView::u32_col(std::string_view name) const noexcept {
  const RaColumnDescriptor *cd = find_col(name, RaDType::U32);
  if (cd == nullptr) {
    return {};
  }
  return {reinterpret_cast<const std::uint32_t *>(base_ + cd->data_offset),
          static_cast<std::size_t>(header_.n_rows)};
}

std::span<const std::uint8_t> RaSectionView::u8enum_col(std::string_view name) const noexcept {
  const RaColumnDescriptor *cd = find_col(name, RaDType::U8Enum);
  if (cd == nullptr) {
    return {};
  }
  return {reinterpret_cast<const std::uint8_t *>(base_ + cd->data_offset),
          static_cast<std::size_t>(header_.n_rows)};
}

RaStringTable RaSectionView::u8enum_labels(std::string_view name) const noexcept {
  const RaColumnDescriptor *cd = find_col(name, RaDType::U8Enum);
  if (cd == nullptr) {
    return {};
  }
  return string_table(*cd);
}

RaDictColumn RaSectionView::dict_col(std::string_view name) const noexcept {
  const RaColumnDescriptor *cd = find_col(name, RaDType::DictStr);
  if (cd == nullptr) {
    return {};
  }
  const std::span<const std::uint32_t> codes{
      reinterpret_cast<const std::uint32_t *>(base_ + cd->data_offset),
      static_cast<std::size_t>(header_.n_rows)};
  return RaDictColumn{codes, string_table(*cd)};
}

} // namespace atx::vol
