#pragma once

// PersistFile (ATXVPF) on-disk vocabulary: constants, the typed refusal currency, the
// three framing records and the schema fingerprint. The authoritative layout spec is
// atx-vol/docs/storage/PERSIST_FORMAT.md; every size and offset it states is pinned
// below by static_assert, so a layout drift is a compile error in every consumer.
//
// PersistFile is the ONE generic container for the artifacts atx-vol persists that
// have no home yet (market states, risk runs, uid-major surface history; design
// wave3-persist D1/D2). It reuses ATXVFA's framing (256 B header, 64 B section
// entries, 64-byte aligned payloads, 64 B footer, the same validation order) and adds
// a kind, a schema fingerprint, per-section dtype / codec / filter / raw length / raw
// CRC, a Lazy verify level and a file size padded to a 4096 multiple.
//
// Every struct here is trivially copyable, standard layout, little-endian, naturally
// aligned and free of implicit padding: each gap is an explicit `reserved` member
// that writers zero and readers verify. Pure vocabulary: no IO.
//
// FROZEN API CONTRACT (wave 3, lane P scaffold 2026-10-24). Lanes may ADD; changing a
// declaration goes back to the orchestrator.
//
// Tier-B: public and includable, deliberately outside the frozen umbrella.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

#include <tl/expected.hpp>

#include "atx/vol/api/core/types.hpp" // Status

namespace atx::vol {

// ── Constants ────────────────────────────────────────────────────────────────

// "\x89VPF\r\n\x1A\n": the high bit, CRLF and ^Z catch 7-bit and text-mode transfer
// damage (the PNG signature idea, as ATXVFA).
inline constexpr std::array<std::uint8_t, 8> kPersistMagic{0x89, 0x56, 0x50, 0x46,
                                                           0x0D, 0x0A, 0x1A, 0x0A};
// "VPF-END\n"
inline constexpr std::array<std::uint8_t, 8> kPersistFooterMagic{0x56, 0x50, 0x46, 0x2D,
                                                                 0x45, 0x4E, 0x44, 0x0A};
// The framing version. A reader refuses any other major (UnsupportedMajor); layouts
// of section CONTENT are versioned by the per-kind schema fingerprint instead.
inline constexpr std::uint16_t kPersistFormatMajor = 1;
inline constexpr std::uint16_t kPersistFormatMinor = 0;
inline constexpr std::uint32_t kPersistEndianTag = 0x01020304u;
inline constexpr std::uint64_t kPersistSectionAlign = 64;
// file_size is a multiple of this, so an unbuffered (sector-aligned) read is legal.
inline constexpr std::uint64_t kPersistFileAlign = 4096;
inline constexpr std::uint32_t kPersistMaxSections = 256;
inline constexpr std::size_t kPersistWriterNameMax = 32;
// "No section" / "no row".
inline constexpr std::uint32_t kPersistNone = 0xFFFFFFFFu;
// PersistSectionEntry::flags bit 0: a reader that does not know the type must refuse.
inline constexpr std::uint32_t kPersistSectionRequired = 1u;

enum class PersistKind : std::uint32_t {
  Manifest = 1,        // a directory's single mutable pointer (PersistDirectory)
  MarketState = 2,     // .atxms  (state_archive.hpp)
  SurfaceTimeline = 3, // .atxstl (surface_timeline.hpp)
  RiskRun = 4,         // .atxrr  (risk_run_archive.hpp)
};

// Element type of a section. Scalar dtypes fix the stride to their size; `Bytes` is an
// opaque blob (stride 1) and `Pod` a fixed-stride record whose field list is folded
// into the kind's schema fingerprint.
enum class PersistDType : std::uint8_t { Bytes = 0, U8, U16, U32, U64, I32, I64, F32, F64, Pod };
inline constexpr std::uint8_t kPersistDTypeCount = 10;

enum class PersistCodec : std::uint8_t { None = 0, Zstd = 1 };
inline constexpr std::uint8_t kPersistCodecCount = 2;
// Applied before the codec (and undone after it): ByteShuffle transposes the bytes of
// each element by significance (Blosc), so the f32-origin zero low mantissa bytes of
// vendor doubles form long runs.
enum class PersistFilter : std::uint8_t { None = 0, ByteShuffle = 1 };
inline constexpr std::uint8_t kPersistFilterCount = 2;

// Open-time integrity work (design persist D7):
//   Framing  header, footer and section-table CRCs plus the structural pass
//   Lazy     + each section's CRC on its first `section()` touch (atomic verdict)
//   Full     + every section CRC and the padding scan at open
//   Paranoid + the whole-file CRC in the footer
// Compressed sections are always decoded and verified at open.
enum class PersistVerify : std::uint8_t { Framing = 0, Lazy = 1, Full = 2, Paranoid = 3 };

// ── Typed refusal currency (no strings: a refusal never allocates) ──────────

enum class PersistStatus : std::uint16_t {
  Ok = 0,
  InvalidArgument,         // writer input or API misuse
  NotFound,                // file, directory, key or section absent
  IoError,                 // OS read / write / rename failure
  AlreadyExists,           // publish onto an existing path
  Busy,                    // a directory lock held by a live writer
  Truncated,               // shorter than the header claims, or cut before the footer
  SizeMismatch,            // longer than the header claims / than the manifest says
  BadMagic,                // not an ATXVPF file
  UnsupportedEndian,       // endian tag does not read back
  UnsupportedMajor,        // format major differs from this reader's
  WrongKind,               // a valid file of another kind
  SchemaMismatch,          // a valid file of this kind with another schema fingerprint
  HeaderInvalid,           // header sizes / offsets / reserved bytes / alignment
  HeaderChecksum,
  FooterInvalid,           // footer magic / reserved bytes
  FooterChecksum,
  FooterMismatch,          // footer copies disagree with the header
  SectionTableChecksum,
  SectionBounds,           // a section outside the payload area
  SectionOverlap,          // sections out of order or overlapping
  SectionAlignment,        // offset not a multiple of 64
  SectionStride,           // stride / length / count / dtype disagree
  DuplicateSection,
  MissingSection,          // a required section of the schema is absent
  UnsupportedSection,      // unknown type flagged Required, unknown dtype / codec / filter
  UnsupportedSectionMajor, // known type with a different type_major
  SectionChecksum,         // stored bytes
  CodecError,              // the codec refused the stored bytes / decoded length differs
  RawChecksum,             // decoded bytes
  PaddingNotZero,
  FileChecksum,            // whole-file CRC (Paranoid)
  RecordInvalid,           // an element's content is invalid (enum byte, name, range)
  IndexUnsorted,           // keys not strictly increasing
  IndexBounds,             // an index entry names no row / a range outside its column
  Misaligned,              // backing buffer not 8-byte aligned
  DuplicateKey,            // writer: two records with one key
  SegmentMismatch,         // a file differs from its manifest entry
  StaleSource,             // a derived file's source fingerprint no longer matches
};

[[nodiscard]] const char *to_string(PersistStatus status) noexcept;
[[nodiscard]] const char *to_string(PersistKind kind) noexcept;

struct PersistError {
  PersistStatus status{PersistStatus::InvalidArgument};
  std::uint32_t section{kPersistNone}; // section type involved, or kPersistNone
  std::uint64_t detail{0};             // element index / byte offset (status-specific)
};

template <class T> using PersistResult = tl::expected<T, PersistError>;
using PersistOutcome = PersistResult<void>;

[[nodiscard]] inline tl::unexpected<PersistError>
persist_error(PersistStatus status, std::uint32_t section = kPersistNone,
              std::uint64_t detail = 0) noexcept {
  return tl::unexpected<PersistError>(PersistError{status, section, detail});
}

// Bridge into house `Result` / `Status` callers. The message names the status, the
// section and the detail; NotFound / IoError / InvalidArgument / AlreadyExists keep
// their ErrorCode, every integrity refusal maps to ParseError. Allocates (a string).
[[nodiscard]] Status to_status(const PersistError &error);

// ── Framing records ──────────────────────────────────────────────────────────

struct PersistHeader {
  std::array<std::uint8_t, 8> magic{};
  std::uint16_t format_major{};
  std::uint16_t format_minor{};
  std::uint16_t header_size{};        // 256
  std::uint16_t section_entry_size{}; // 64
  std::uint32_t endian_tag{};
  std::uint32_t footer_size{};        // 64
  std::uint64_t file_size{};          // multiple of kPersistFileAlign
  std::uint64_t section_table_offset{}; // 256
  std::uint32_t section_count{};
  std::uint32_t kind{};               // PersistKind
  std::uint64_t schema_fingerprint{};
  std::uint64_t source_fingerprint{}; // what the content was derived from (0: none)
  std::int64_t created_unix_ns{};     // writer-supplied (0 keeps builds byte-identical)
  std::int64_t ts_min_ns{};           // market-time range the content covers
  std::int64_t ts_max_ns{};
  std::uint64_t writer_version{};     // ATX_VOL_VERSION
  std::array<char, kPersistWriterNameMax> writer_name{}; // NUL padded
  std::uint32_t flags{};              // 0 in v1
  std::uint32_t section_table_crc32c{};
  std::uint32_t header_crc32c{};      // over these 256 bytes with this field zero
  std::uint32_t reserved0{};
  std::array<std::uint8_t, 112> reserved{};
};

struct PersistSectionEntry {
  std::uint32_t type{};
  std::uint16_t type_major{};
  std::uint16_t type_minor{};
  std::uint64_t offset{};        // absolute, multiple of 64
  std::uint64_t length{};        // stored bytes (the codec frame when codec != None)
  std::uint64_t element_count{}; // decoded elements: raw_length / element_stride
  std::uint64_t raw_length{};    // decoded bytes (== length when codec == None)
  std::uint32_t element_stride{};
  std::uint32_t flags{};         // kPersistSectionRequired
  std::uint32_t crc32c{};        // over the stored bytes
  std::uint32_t raw_crc32c{};    // over the decoded bytes (== crc32c when codec == None)
  std::uint8_t dtype{};          // PersistDType
  std::uint8_t codec{};          // PersistCodec
  std::uint8_t filter{};         // PersistFilter (None when codec == None)
  std::uint8_t reserved0{};
  std::uint32_t reserved1{};
};

struct PersistFooter {
  std::array<std::uint8_t, 8> magic{};
  std::uint64_t file_size{};
  std::uint64_t section_table_offset{};
  std::uint32_t section_count{};
  std::uint32_t kind{};
  std::uint64_t schema_fingerprint{};
  std::uint32_t header_crc32c{};
  std::uint32_t file_crc32c{};   // over [0, file_size - 64)
  std::uint32_t footer_crc32c{}; // over these 64 bytes with this field zero
  std::uint32_t reserved0{};
  std::uint64_t reserved1{};
};

template <class T> inline constexpr bool kPersistPod =
    std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;

static_assert(kPersistPod<PersistHeader> && sizeof(PersistHeader) == 256);
static_assert(offsetof(PersistHeader, file_size) == 24);
static_assert(offsetof(PersistHeader, kind) == 44);
static_assert(offsetof(PersistHeader, schema_fingerprint) == 48);
static_assert(offsetof(PersistHeader, source_fingerprint) == 56);
static_assert(offsetof(PersistHeader, created_unix_ns) == 64);
static_assert(offsetof(PersistHeader, writer_version) == 88);
static_assert(offsetof(PersistHeader, writer_name) == 96);
static_assert(offsetof(PersistHeader, flags) == 128);
static_assert(offsetof(PersistHeader, header_crc32c) == 136);
static_assert(offsetof(PersistHeader, reserved) == 144);
static_assert(kPersistPod<PersistSectionEntry> && sizeof(PersistSectionEntry) == 64);
static_assert(offsetof(PersistSectionEntry, raw_length) == 32);
static_assert(offsetof(PersistSectionEntry, element_stride) == 40);
static_assert(offsetof(PersistSectionEntry, crc32c) == 48);
static_assert(offsetof(PersistSectionEntry, dtype) == 56);
static_assert(offsetof(PersistSectionEntry, reserved1) == 60);
static_assert(kPersistPod<PersistFooter> && sizeof(PersistFooter) == 64);
static_assert(offsetof(PersistFooter, schema_fingerprint) == 32);
static_assert(offsetof(PersistFooter, file_crc32c) == 44);
static_assert(offsetof(PersistFooter, footer_crc32c) == 48);

// Size of one element of a scalar dtype; 0 for Bytes / Pod (the stride decides) and
// for an out-of-range value.
[[nodiscard]] constexpr std::uint32_t persist_dtype_size(PersistDType dtype) noexcept {
  switch (dtype) {
  case PersistDType::Bytes: return 0;
  case PersistDType::U8: return 1;
  case PersistDType::U16: return 2;
  case PersistDType::U32: return 4;
  case PersistDType::U64: return 8;
  case PersistDType::I32: return 4;
  case PersistDType::I64: return 8;
  case PersistDType::F32: return 4;
  case PersistDType::F64: return 8;
  case PersistDType::Pod: return 0;
  }
  return 0;
}

// The dtype a typed view of T reads: the scalar for arithmetic types (an enum reads as
// its underlying type), Bytes for std::byte / char, Pod for every other class type.
template <class T> [[nodiscard]] constexpr PersistDType persist_dtype_of() noexcept {
  if constexpr (std::is_enum_v<T>) {
    return persist_dtype_of<std::underlying_type_t<T>>();
  } else if constexpr (std::is_same_v<T, std::byte> || std::is_same_v<T, char>) {
    return PersistDType::Bytes;
  } else if constexpr (std::is_same_v<T, std::uint8_t>) {
    return PersistDType::U8;
  } else if constexpr (std::is_same_v<T, std::uint16_t>) {
    return PersistDType::U16;
  } else if constexpr (std::is_same_v<T, std::uint32_t>) {
    return PersistDType::U32;
  } else if constexpr (std::is_same_v<T, std::uint64_t>) {
    return PersistDType::U64;
  } else if constexpr (std::is_same_v<T, std::int32_t>) {
    return PersistDType::I32;
  } else if constexpr (std::is_same_v<T, std::int64_t>) {
    return PersistDType::I64;
  } else if constexpr (std::is_same_v<T, float>) {
    return PersistDType::F32;
  } else if constexpr (std::is_same_v<T, double>) {
    return PersistDType::F64;
  } else {
    return PersistDType::Pod;
  }
}

// ── Schema fingerprint (design persist D5) ───────────────────────────────────
//
// A reader accepts exactly ONE fingerprint per kind: any layout change (a section
// added / removed / retyped / restrided, a POD field moved or resized) changes it and
// the file is refused with SchemaMismatch. The fix is to rebuild; there is no
// backward compatibility. The arithmetic is FNV-1a/64 over a canonical little-endian
// serialization, then the MurmurHash3 fmix64 finalizer (the ATXVSA2 symbol-hash
// arithmetic), so it is a pure constexpr function of the spec tables.

struct PersistFieldSpec {
  std::string_view name{};
  std::uint32_t offset{0};
  std::uint32_t size{0};
};

struct PersistSectionSpec {
  std::uint32_t type{0};
  std::uint16_t type_major{1};
  PersistDType dtype{PersistDType::Bytes};
  std::uint32_t stride{1};
  std::string_view name{};
  std::span<const PersistFieldSpec> fields{}; // Pod only: every field, in offset order
  bool required{true};
};

namespace persist_detail {
inline constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ull;
inline constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;

[[nodiscard]] constexpr std::uint64_t fold_le(std::uint64_t h, std::uint64_t v,
                                              unsigned bytes) noexcept {
  for (unsigned i = 0; i < bytes; ++i) {
    h ^= (v >> (8u * i)) & 0xFFu;
    h *= kFnvPrime;
  }
  return h;
}

// Length-prefixed, so ("ab", "c") and ("a", "bc") fold differently.
[[nodiscard]] constexpr std::uint64_t fold_str(std::uint64_t h, std::string_view s) noexcept {
  h = fold_le(h, s.size(), 4);
  for (const char c : s) {
    h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
    h *= kFnvPrime;
  }
  return h;
}

[[nodiscard]] constexpr std::uint64_t fmix64(std::uint64_t h) noexcept {
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdull;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ull;
  h ^= h >> 33;
  return h;
}
} // namespace persist_detail

// The 8 ASCII characters of a kind salt ("ATXVMS01") as a little-endian u64.
[[nodiscard]] constexpr std::uint64_t persist_salt(std::string_view eight) noexcept {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < eight.size() && i < 8; ++i) {
    v |= static_cast<std::uint64_t>(static_cast<unsigned char>(eight[i])) << (8u * i);
  }
  return v;
}

[[nodiscard]] constexpr std::uint64_t
persist_fingerprint(std::uint64_t salt, std::span<const PersistSectionSpec> sections) noexcept {
  using persist_detail::fold_le;
  using persist_detail::fold_str;
  std::uint64_t h = persist_detail::kFnvOffset;
  h = fold_le(h, salt, 8);
  h = fold_le(h, sections.size(), 4);
  for (const PersistSectionSpec &s : sections) {
    h = fold_le(h, s.type, 4);
    h = fold_le(h, s.type_major, 2);
    h = fold_le(h, static_cast<std::uint8_t>(s.dtype), 1);
    h = fold_le(h, s.stride, 4);
    h = fold_le(h, s.required ? 1u : 0u, 1);
    h = fold_str(h, s.name);
    h = fold_le(h, s.fields.size(), 4);
    for (const PersistFieldSpec &f : s.fields) {
      h = fold_str(h, f.name);
      h = fold_le(h, f.offset, 4);
      h = fold_le(h, f.size, 4);
    }
  }
  return persist_detail::fmix64(h);
}

// True iff `fields` tile [0, stride) exactly: ascending, contiguous, no gap, no
// overlap. A POD section type proves it has no implicit padding with
//   static_assert(persist_fields_tile(kFields, sizeof(T)));
// where kFields is built from offsetof / sizeof of every member.
[[nodiscard]] constexpr bool persist_fields_tile(std::span<const PersistFieldSpec> fields,
                                                 std::uint32_t stride) noexcept {
  std::uint32_t at = 0;
  for (const PersistFieldSpec &f : fields) {
    if (f.offset != at || f.size == 0) return false;
    at += f.size;
  }
  return at == stride;
}

} // namespace atx::vol
