#pragma once

// PersistFile (ATXVPF): the generic builder, the durable publish, the validating
// zero-copy reader and the directory / manifest pattern. Typed stores (market states,
// surface timelines, risk runs) sit on top of it. Spec: docs/storage/PERSIST_FORMAT.md.
//
// READ PATH CONTRACT. No exceptions for any byte content: every refusal is a
// PersistError. Nothing allocates after `open` returns (section(), typed(),
// verify_all() and the accessors are allocation-free; compressed sections are decoded
// at open). Every read is bounds-checked before a span is formed.
//
// LIFETIME. A PersistFile is move-only (PIMPL), immutable and safe to query from any
// number of threads (the Lazy CRC verdict is the one shared mutable datum: a seq_cst
// atomic per section, idempotent). Every span it returns borrows its backing bytes (a
// read-only mapping or an owned buffer) and dies with the PersistFile; moving the
// object keeps the spans valid. `keepalive()` hands out the owner for a consumer that
// must outlive the object (a future numpy base).
//
// WINDOWS. A published file is never rewritten: `Mapping` opens with FILE_SHARE_READ
// only, so a mapped file cannot be truncated, deleted or replaced. New content gets a
// new name; a directory's single mutable pointer is its manifest, which is read into
// memory (never mapped) and replaced by temp + flush + rename (design persist D8).
//
// FROZEN API CONTRACT (wave 3, lane P scaffold 2026-10-24). Lanes may ADD.
//
// Tier-B: public and includable, deliberately outside the frozen umbrella.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "atx/vol/api/persist/persist_format.hpp"

namespace atx::vol {

// CRC-32C (Castagnoli) of `bytes`: the container's only checksum (the shared
// hardware-dispatched implementation behind ATXVFA / ATXVSA2 / ATXRUN01).
[[nodiscard]] std::uint32_t persist_crc32c(std::span<const std::byte> bytes) noexcept;

// ── Builder ──────────────────────────────────────────────────────────────────

struct PersistSectionInput {
  std::uint32_t type{0};
  std::uint16_t type_major{1};
  std::uint16_t type_minor{0};
  PersistDType dtype{PersistDType::Bytes};
  std::uint32_t stride{1}; // == persist_dtype_size(dtype) for scalar dtypes
  bool required{true};
  PersistCodec codec{};
  PersistFilter filter{}; // ignored unless codec != None
  int level{3};           // zstd level, [1, 19]
  std::span<const std::byte> bytes{}; // borrowed for the build call only
};

struct PersistFileInfo {
  PersistKind kind{PersistKind::Manifest};
  std::uint64_t schema_fingerprint{0};
  std::int64_t created_unix_ns{0}; // 0 keeps two builds of the same content byte-identical
  std::int64_t ts_min_ns{0};
  std::int64_t ts_max_ns{0};
  std::uint64_t source_fingerprint{0};
  std::string_view writer{}; // <= kPersistWriterNameMax bytes; empty -> "atx-vol <version>"
};

// Frame `sections` (in the given order) into one complete image: header, section
// table, 64-byte aligned zero-padded payloads, a zero pad to a 4096 multiple, footer,
// every checksum. Does not interpret section contents, so tools and tests can frame
// arbitrary (including deliberately malformed) payloads. Codec sections are encoded
// on up to `n_threads` threads through pricing_executor() into disjoint buffers: the
// bytes are identical for any thread count (and, for zstd, for a given zstd version).
// InvalidArgument on a duplicate type (DuplicateSection), a stride / dtype / size
// mismatch, an unknown dtype / codec / filter, a bad level, > 256 sections, a writer
// name that does not fit, or ts_min_ns > ts_max_ns; CodecError when zstd refuses.
[[nodiscard]] PersistResult<std::vector<std::byte>>
build_persist_file(std::span<const PersistSectionInput> sections, const PersistFileInfo &info,
                   unsigned n_threads = 1);

// Durable publish of a complete image onto a path that must NOT exist: unique temp
// beside `path`, one fsync, rename (persist D8: published files are immutable).
// AlreadyExists if `path` exists; IoError on an OS failure (no file at `path` then).
// The existence check and the rename are not atomic across processes: concurrent
// publishers serialize on the directory lock (PersistDirectory::publish).
[[nodiscard]] PersistOutcome publish_persist_file(std::string_view path,
                                                  std::span<const std::byte> bytes);

// ── Reader ───────────────────────────────────────────────────────────────────

struct PersistOpenOptions {
  // true: read-only memory map (atx::tsdb::Mapping), pages fault in on use.
  // false: one sized sequential read into an owned 64-byte aligned buffer (for
  // removable / network media, where a device error on a mapped page would raise
  // EXCEPTION_IN_PAGE_ERROR).
  bool mapped{true};
  PersistVerify verify{PersistVerify::Lazy};
  bool prefetch{false};          // mapped only: PrefetchVirtualMemory the whole file
  std::uint64_t expected_size{0}; // nonzero: the file must be exactly this long
};

class PersistFile {
public:
  // Open and validate `path` as a file of `kind` with schema `fingerprint`. NotFound /
  // IoError for the OS, WrongKind / SchemaMismatch for a valid foreign file, otherwise
  // the first validation refusal (PERSIST_FORMAT.md "Validation order"). Allocates;
  // nothing after it returns does.
  [[nodiscard]] static PersistResult<PersistFile> open(std::string_view path, PersistKind kind,
                                                       std::uint64_t fingerprint,
                                                       const PersistOpenOptions &options = {});

  // Validate externally owned bytes; `owner` keeps them alive for the file's lifetime
  // (may be null when the caller guarantees that). The span must be 8-byte aligned
  // (Misaligned otherwise).
  [[nodiscard]] static PersistResult<PersistFile>
  open_bytes(std::span<const std::byte> bytes, std::shared_ptr<const void> owner, PersistKind kind,
             std::uint64_t fingerprint, PersistVerify verify = PersistVerify::Full);

  PersistFile(PersistFile &&) noexcept;
  PersistFile &operator=(PersistFile &&) noexcept;
  PersistFile(const PersistFile &) = delete;
  PersistFile &operator=(const PersistFile &) = delete;
  ~PersistFile();

  [[nodiscard]] const PersistHeader &header() const noexcept;
  [[nodiscard]] const PersistFooter &footer() const noexcept;
  // The section table in file order.
  [[nodiscard]] std::span<const PersistSectionEntry> sections() const noexcept;
  // The whole backing image.
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
  // Table entry of `type`, nullptr when absent.
  [[nodiscard]] const PersistSectionEntry *entry(std::uint32_t type) const noexcept;

  // The (decoded) payload of `type`. MissingSection when absent; under Lazy the first
  // touch verifies the stored CRC (SectionChecksum on mismatch, and every later touch
  // reports the same verdict).
  [[nodiscard]] PersistResult<std::span<const std::byte>> section(std::uint32_t type) const noexcept;

  // Typed zero-copy view. SectionStride when the stored stride is not sizeof(T) or the
  // stored dtype is not persist_dtype_of<T>(); Misaligned when the payload address is
  // not a multiple of alignof(T). Content is NOT validated here (enum bytes, ranges):
  // the kind's reader does that at open.
  template <class T>
  [[nodiscard]] PersistResult<std::span<const T>> typed(std::uint32_t type) const noexcept {
    static_assert(kPersistPod<T>, "typed<T>: T must be trivially copyable standard layout");
    const PersistSectionEntry *e = entry(type);
    if (e == nullptr) return persist_error(PersistStatus::MissingSection, type);
    if (e->element_stride != sizeof(T) ||
        e->dtype != static_cast<std::uint8_t>(persist_dtype_of<T>())) {
      return persist_error(PersistStatus::SectionStride, type, e->element_stride);
    }
    PersistResult<std::span<const std::byte>> b = section(type);
    if (!b) return tl::unexpected(b.error());
    // SAFETY: address arithmetic only, to check alignment.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (reinterpret_cast<std::uintptr_t>(b->data()) % alignof(T) != 0u) {
      return persist_error(PersistStatus::Misaligned, type);
    }
    // SAFETY: the Arrow / FlatBuffers mmap idiom (ATXVFA view<T>). open() proved the
    // section lies inside the backing, its offset is a multiple of 64, its length is
    // element_count * stride, and the check above proves stride == sizeof(T) and the
    // address is aligned for T; T is trivially copyable (an implicit-lifetime type),
    // so the bytes are read as T in place.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return std::span<const T>{reinterpret_cast<const T *>(b->data()), b->size() / sizeof(T)};
  }

  // Check every section against a kind's schema table: a required spec absent ->
  // MissingSection; a present known section with another type_major ->
  // UnsupportedSectionMajor, another dtype / stride -> SectionStride; a present
  // UNKNOWN section flagged Required -> UnsupportedSection; an unknown optional section
  // is skipped. Allocation-free.
  [[nodiscard]] PersistOutcome check_schema(std::span<const PersistSectionSpec> schema) const noexcept;

  // Verify every not-yet-verified section CRC and the whole-file CRC now (what a
  // `verify` CLI calls on a Framing / Lazy open). Allocation-free.
  [[nodiscard]] PersistOutcome verify_all() const noexcept;

  // The owner of the backing bytes (a Mapping or an owned buffer).
  [[nodiscard]] std::shared_ptr<const void> keepalive() const noexcept;

private:
  struct Impl;
  explicit PersistFile(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

// ── Directory + manifest ─────────────────────────────────────────────────────

// One published file of a directory. NUL-padded names; `file_name` is exactly
// persist_file_name(kind, key, seq) (anything else is RecordInvalid at open).
struct PersistManifestEntry {
  std::uint64_t seq{};                 // directory-unique, ascending in publish order
  std::array<char, 48> file_name{};
  std::array<char, 16> key{};          // [A-Za-z0-9_.-]{1,16}, unique within the manifest
  std::uint64_t file_size{};
  std::uint32_t file_crc32c{};         // the file footer's whole-file CRC
  std::uint32_t header_crc32c{};
  std::uint32_t kind{};                // PersistKind
  std::uint32_t records{};             // kind-specific count (marks, entries, steps)
  std::uint64_t schema_fingerprint{};
  std::uint64_t source_fingerprint{};
  std::int64_t ts_min_ns{};
  std::int64_t ts_max_ns{};
};
static_assert(kPersistPod<PersistManifestEntry> && sizeof(PersistManifestEntry) == 128);
static_assert(offsetof(PersistManifestEntry, key) == 56);
static_assert(offsetof(PersistManifestEntry, file_size) == 72);
static_assert(offsetof(PersistManifestEntry, kind) == 88);
static_assert(offsetof(PersistManifestEntry, schema_fingerprint) == 96);

// Manifest kind: salt "ATXVMF01"; section 1 = PersistManifestEntry rows sorted by key.
inline constexpr std::uint32_t kPersistManifestEntriesSection = 1;
inline constexpr std::string_view kPersistManifestFileName = "persist-manifest.atxvpm";
inline constexpr std::string_view kPersistLockFileName = "persist.lock";
[[nodiscard]] std::uint64_t persist_manifest_fingerprint() noexcept;

// The canonical file name of `key` at publish sequence `seq`:
//   MarketState      bucket_et=<key>-g<seq, 12 digits>.atxms
//   SurfaceTimeline  <key>-g<seq, 12 digits>.atxstl
//   RiskRun          seg-<seq, 12 digits>.atxrr
// InvalidArgument for the Manifest kind or a key outside [A-Za-z0-9_.-]{1,16} (and
// never "." / ".."): the name can never carry a path part.
[[nodiscard]] PersistResult<std::string> persist_file_name(PersistKind kind, std::string_view key,
                                                           std::uint64_t seq);

class PersistDirectory {
public:
  // Read `dir`'s manifest into memory (Read mode, Paranoid) and validate every entry
  // (sorted unique keys, unique seqs, canonical file names). NotFound when the
  // directory or its manifest is absent. Files on disk that the manifest does not
  // list (crash orphans, superseded generations) are invisible.
  [[nodiscard]] static PersistResult<PersistDirectory> open(std::string_view dir);

  [[nodiscard]] std::string_view dir() const noexcept { return dir_; }
  // Sorted by key.
  [[nodiscard]] std::span<const PersistManifestEntry> entries() const noexcept {
    return entries_;
  }
  // nullptr when absent. O(log n).
  [[nodiscard]] const PersistManifestEntry *find(std::string_view key) const noexcept;
  // `dir()/file_name` of an entry.
  [[nodiscard]] std::string path(const PersistManifestEntry &entry) const;

  // Publish `image` (a complete PersistFile, validated here at Framing level) as the
  // new generation of `key` in `dir` (created if absent): take `dir/persist.lock`
  // (WriterLock; Busy when a live writer holds it past the timeout), read the
  // manifest, publish the data file under a fresh name (persist_file_name with
  // seq = max seq + 1), then replace the manifest (temp + flush + rename). A crash in
  // between leaves an unreferenced orphan, never a dangling entry. The previous
  // generation's file stays on disk (it may be mapped by a reader). `records` is
  // stored in the entry as is.
  [[nodiscard]] static PersistResult<PersistManifestEntry>
  publish(std::string_view dir, std::string_view key, std::span<const std::byte> image,
          std::uint32_t records = 0);

private:
  std::string dir_;
  std::vector<PersistManifestEntry> entries_;
};

} // namespace atx::vol
