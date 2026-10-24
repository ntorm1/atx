#pragma once

// ATXVFA container: low-level builder, atomic publish and the validating,
// zero-copy reader. Spec: atx-vol/docs/storage/ARCHIVE_FORMAT.md. Typed record
// encoding / decoding against the library's fit, ExpiryRecord and Markov types
// is in fit_archive_records.hpp; snapshot series in fit_archive_series.hpp.
//
// Read path contract: no exceptions for any byte content (every refusal is an
// ArchiveError carrying an ArchiveStatus), and no allocation after `open`
// returns. Thread safety: an open FitArchive is immutable; every member is const
// and safe to call from any number of threads. Views (spans, pointers, string
// views) borrow the archive's backing bytes and die with the FitArchive object
// (moves keep them valid; the type is move-only).
//
// Tier-B: public and includable, deliberately outside the frozen umbrella.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include "atx/vol/api/storage/fit_archive_format.hpp"

namespace atx::vol {

// CRC-32C (Castagnoli) of `bytes`, the archive's only checksum (shared with
// ATXVSA2 / ATXVDB: hardware dispatched, bit-identical table fallback).
[[nodiscard]] std::uint32_t fit_archive_crc32c(std::span<const std::byte> bytes) noexcept;

// ── Low-level builder ────────────────────────────────────────────────────────

struct FitArchiveSectionInput {
  FitArchiveSectionType type{FitArchiveSectionType::Strings};
  std::uint16_t type_major{1};
  std::uint16_t type_minor{0};
  std::uint32_t element_stride{1}; // bytes.size() must be a multiple
  std::uint32_t element_align{1};  // 1 or 8
  std::uint32_t flags{kFitArchiveSectionRequired};
  std::span<const std::byte> bytes{}; // borrowed for the build call only
};

struct FitArchiveFileInfo {
  std::int64_t created_unix_ns{0};
  std::uint64_t archive_id{0};
  std::uint64_t segment_sequence{0};
};

// Frame `sections` (in the given order) into one complete archive image: header,
// section table, 64-byte aligned zero-padded payloads, footer, every checksum.
// Does not interpret section contents, so tools and tests can frame arbitrary
// (including deliberately malformed) payloads. InvalidArgument on a duplicate
// type, a stride/size mismatch, an alignment other than 1/8, or > 256 sections.
[[nodiscard]] ArchiveResult<std::vector<std::byte>>
build_fit_archive(std::span<const FitArchiveSectionInput> sections, const FitArchiveFileInfo &info);

// Durable atomic publish of a complete image: unique temp beside `path`, one
// fsync of the temp, rename onto `path` (bounded retry while a reader holds it).
// IoError on failure; the previous file at `path` is then intact.
[[nodiscard]] ArchiveOutcome publish_fit_archive(std::string_view path,
                                                 std::span<const std::byte> bytes);

// ── Reader ───────────────────────────────────────────────────────────────────

enum class FitArchiveOpenMode : std::uint8_t {
  Mapped, // read-only memory map (atx::tsdb::Mapping); pages fault in on use
  Read,   // one sized read into an owned 8-byte aligned buffer
};

// Open cost (ARCHIVE_FORMAT.md "Open cost model"): every level runs the framing
// checks and the O(records) structural record pass; `Full` adds an O(file) CRC and
// padding scan (a `Mapped` open faults in every page); `Paranoid` a second O(file)
// CRC. Lookups after the open are binary searches.
enum class FitArchiveVerify : std::uint8_t {
  Framing,  // header / footer / table checksums and the full structural pass;
            // skips section checksums and the padding scan (trusted cache)
  Full,     // + every section checksum and every padding byte (default)
  Paranoid, // + the whole-file checksum in the footer
};

struct FitArchiveOpenOptions {
  FitArchiveOpenMode mode{FitArchiveOpenMode::Mapped};
  FitArchiveVerify verify{FitArchiveVerify::Full};
};

class FitArchive {
public:
  // Open and validate `path`. NotFound / IoError for the OS, otherwise the first
  // validation refusal (ARCHIVE_FORMAT.md section 8). Allocates; nothing after
  // this returns does.
  [[nodiscard]] static ArchiveResult<FitArchive> open(std::string_view path,
                                                      const FitArchiveOpenOptions &options = {});

  // Validate externally owned bytes; `owner` keeps them alive for the archive's
  // lifetime (may be null when the caller guarantees that). The span must be
  // 8-byte aligned (Misaligned otherwise).
  [[nodiscard]] static ArchiveResult<FitArchive>
  open_bytes(std::span<const std::byte> bytes, std::shared_ptr<const void> owner,
             FitArchiveVerify verify = FitArchiveVerify::Full);

  // Copy `bytes` once into an aligned owned buffer, then validate.
  [[nodiscard]] static ArchiveResult<FitArchive>
  open_copy(std::span<const std::byte> bytes, FitArchiveVerify verify = FitArchiveVerify::Full);

  FitArchive(FitArchive &&) noexcept = default;
  FitArchive &operator=(FitArchive &&) noexcept = default;
  FitArchive(const FitArchive &) = delete;
  FitArchive &operator=(const FitArchive &) = delete;
  ~FitArchive() = default;

  // ── Framing ──
  [[nodiscard]] const FitArchiveHeader &header() const noexcept { return header_; }
  [[nodiscard]] const FitArchiveFooter &footer() const noexcept { return footer_; }
  [[nodiscard]] std::span<const FitArchiveSectionEntry> sections() const noexcept {
    return sections_;
  }
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }
  // Table entry of `type`, nullptr when absent.
  [[nodiscard]] const FitArchiveSectionEntry *section(FitArchiveSectionType type) const noexcept;
  // Payload of `type` as served to views (an upgraded copy when the stored
  // stride differs from this reader's), empty when absent.
  [[nodiscard]] std::span<const std::byte> section_bytes(FitArchiveSectionType type) const noexcept;
  // Recompute one stored section's checksum / the whole-file checksum (the
  // `verify` CLI localises damage with these). Allocation-free.
  [[nodiscard]] ArchiveOutcome verify_section(std::size_t table_index) const noexcept;
  [[nodiscard]] ArchiveOutcome verify_file_checksum() const noexcept;

  // ── Symbols, strings, provenance ──
  [[nodiscard]] std::span<const FitArchiveSymbol> symbols() const noexcept {
    return view<FitArchiveSymbol>(FitArchiveSectionType::Symbols);
  }
  [[nodiscard]] std::optional<std::uint32_t> find_underlier(std::string_view name) const noexcept;
  [[nodiscard]] std::string_view underlier_name(std::uint32_t id) const noexcept;
  // Empty view for an out-of-range reference (validated records never have one).
  [[nodiscard]] std::string_view string(FitArchiveStringRef ref) const noexcept;
  [[nodiscard]] std::span<const std::byte> provenance() const noexcept {
    return section_bytes(FitArchiveSectionType::Provenance);
  }

  // ── Keyed records, in writer insertion order (index_of() gives key order) ──
  [[nodiscard]] std::span<const FitArchiveSurfaceRecord> surfaces() const noexcept {
    return view<FitArchiveSurfaceRecord>(FitArchiveSectionType::SurfaceRecords);
  }
  [[nodiscard]] std::span<const FitArchiveFitRecord> fits() const noexcept {
    return view<FitArchiveFitRecord>(FitArchiveSectionType::FitRecords);
  }
  [[nodiscard]] std::span<const FitArchiveExpiryMeta> expiry_meta() const noexcept {
    return view<FitArchiveExpiryMeta>(FitArchiveSectionType::ExpiryMeta);
  }
  [[nodiscard]] std::span<const FitArchiveLadderRecord> ladders() const noexcept {
    return view<FitArchiveLadderRecord>(FitArchiveSectionType::Ladders);
  }
  [[nodiscard]] std::span<const FitArchiveMarkovRecord> markov() const noexcept {
    return view<FitArchiveMarkovRecord>(FitArchiveSectionType::MarkovRecords);
  }

  // O(log n) binary search of the record type's index, then O(1) addressing.
  // nullptr when absent.
  [[nodiscard]] const FitArchiveSurfaceRecord *find_surface(std::uint32_t underlier,
                                                            std::int64_t snapshot_ns) const noexcept;
  [[nodiscard]] const FitArchiveFitRecord *find_fit(const FitArchiveKey &key) const noexcept;
  [[nodiscard]] const FitArchiveExpiryMeta *find_expiry(const FitArchiveKey &key) const noexcept;
  [[nodiscard]] const FitArchiveLadderRecord *find_ladder(const FitArchiveKey &key) const noexcept;
  [[nodiscard]] const FitArchiveMarkovRecord *find_markov(const FitArchiveKey &key) const noexcept;

  // The expiries of one surface, in expiry order.
  [[nodiscard]] std::span<const FitArchiveFitRecord>
  fits_of(const FitArchiveSurfaceRecord &surface) const noexcept;

  // ── Archive-native pools ──
  [[nodiscard]] std::span<const FitArchiveQuoteRow>
  quote_rows(const FitArchiveFitRecord &fit) const noexcept;
  [[nodiscard]] std::span<const std::uint8_t> moved_nodes(const FitArchiveFitRecord &fit) const noexcept;
  // QuoteSliceFit::price_only_nodes of `fit` (file minor 1; empty in a minor-0 file).
  [[nodiscard]] std::span<const std::uint8_t>
  price_only_nodes(const FitArchiveFitRecord &fit) const noexcept;
  // The curve payload words (ARCHIVE_FORMAT.md section 6); integer words are
  // u64 bit patterns (read them with std::bit_cast).
  [[nodiscard]] std::span<const double> curve_payload(const FitArchiveFitRecord &fit) const noexcept;
  [[nodiscard]] std::span<const FitArchiveLadderContract>
  ladder_contracts(const FitArchiveLadderRecord &ladder) const noexcept;
  [[nodiscard]] std::span<const FitArchiveExpiryMeta>
  ladder_records(const FitArchiveLadderRecord &ladder) const noexcept;
  [[nodiscard]] std::span<const FitArchiveMarkovInterval>
  markov_intervals(const FitArchiveMarkovRecord &record) const noexcept;
  [[nodiscard]] std::span<const double> markov_words(FitArchiveArrayRef ref) const noexcept;
  [[nodiscard]] std::span<const FitArchiveSeriesSegment> series_segments() const noexcept {
    return view<FitArchiveSeriesSegment>(FitArchiveSectionType::SeriesSegments);
  }

  // The index entries of a keyed record section (SurfaceRecords, FitRecords,
  // ExpiryMeta, Ladders, MarkovRecords); empty otherwise.
  [[nodiscard]] std::span<const FitArchiveIndexEntry>
  index_of(FitArchiveSectionType records) const noexcept;

  // Typed zero-copy view of a section whose served stride equals sizeof(T) (the
  // records layer uses it for pools laid out as library structs). Empty when the
  // section is absent or its stride differs.
  template <class T> [[nodiscard]] std::span<const T> view(FitArchiveSectionType type) const noexcept {
    static_assert(std::is_trivially_copyable_v<T> && alignof(T) <= 8);
    const std::size_t slot = slot_of(type);
    if (slot == kNoSlot || served_[slot].stride != sizeof(T)) return {};
    // SAFETY: the Arrow / FlatBuffers mmap idiom. open() proved the backing is
    // 8-byte aligned, the section offset a multiple of 64 and the stride equal to
    // sizeof(T), and validated every element's content; T is trivially copyable
    // (an implicit-lifetime type), so the bytes are read as T in place.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return {reinterpret_cast<const T *>(served_[slot].bytes.data()), served_[slot].count};
  }

private:
  static constexpr std::size_t kSlots = 96; // section type values are < 96
  static constexpr std::size_t kNoSlot = kSlots;
  struct Served {
    std::span<const std::byte> bytes{};
    std::size_t count{0};
    std::uint32_t stride{0};
    std::uint32_t table_index{kFitArchiveNone};
  };

  FitArchive() = default;
  [[nodiscard]] static ArchiveResult<FitArchive> open_impl(std::span<const std::byte> bytes,
                                                           std::shared_ptr<const void> owner,
                                                           FitArchiveVerify verify);
  [[nodiscard]] std::size_t slot_of(FitArchiveSectionType type) const noexcept;
  template <class R>
  [[nodiscard]] const R *find_keyed(FitArchiveSectionType records, std::span<const R> rows,
                                    const FitArchiveKey &key) const noexcept;
  [[nodiscard]] ArchiveOutcome validate_records() const;

  std::shared_ptr<const void> owner_;
  std::span<const std::byte> bytes_;
  FitArchiveHeader header_{};
  FitArchiveFooter footer_{};
  std::vector<FitArchiveSectionEntry> sections_;
  std::array<Served, kSlots> served_{};
  std::vector<std::vector<std::uint64_t>> upgraded_; // stride-upgraded section copies
};

} // namespace atx::vol
