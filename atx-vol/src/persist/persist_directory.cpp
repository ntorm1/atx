// PersistDirectory: a directory of immutable PersistFiles behind ONE mutable pointer,
// its manifest (design wave3-persist D8, the ATXVFA series pattern). The manifest is a
// PersistFile of kind Manifest, read into memory (never mapped) and replaced by temp +
// flush + rename; writers serialize on `<dir>/persist.lock` (detail::WriterLock) and
// publish the data file BEFORE the manifest, so a crash leaves an orphan, never a
// dangling entry.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/api/persist/persist_file.hpp"
#include "persist/persist_internal.hpp"
#include "storage/writer_lock.hpp"

namespace atx::vol {
namespace {

namespace fs = std::filesystem;

// PersistManifestEntry's field list: the manifest kind's fingerprint folds it, and the
// tile check proves the struct has no implicit padding.
constexpr PersistFieldSpec kManifestFields[] = {
    {"seq", offsetof(PersistManifestEntry, seq), 8},
    {"file_name", offsetof(PersistManifestEntry, file_name), 48},
    {"key", offsetof(PersistManifestEntry, key), 16},
    {"file_size", offsetof(PersistManifestEntry, file_size), 8},
    {"file_crc32c", offsetof(PersistManifestEntry, file_crc32c), 4},
    {"header_crc32c", offsetof(PersistManifestEntry, header_crc32c), 4},
    {"kind", offsetof(PersistManifestEntry, kind), 4},
    {"records", offsetof(PersistManifestEntry, records), 4},
    {"schema_fingerprint", offsetof(PersistManifestEntry, schema_fingerprint), 8},
    {"source_fingerprint", offsetof(PersistManifestEntry, source_fingerprint), 8},
    {"ts_min_ns", offsetof(PersistManifestEntry, ts_min_ns), 8},
    {"ts_max_ns", offsetof(PersistManifestEntry, ts_max_ns), 8},
};
static_assert(persist_fields_tile(kManifestFields, sizeof(PersistManifestEntry)));
constexpr PersistSectionSpec kManifestSchema[] = {
    {kPersistManifestEntriesSection, 1, PersistDType::Pod,
     static_cast<std::uint32_t>(sizeof(PersistManifestEntry)), "manifest-entries", kManifestFields,
     true},
};
constexpr std::uint64_t kManifestFingerprint =
    persist_fingerprint(persist_salt("ATXVMF01"), kManifestSchema);

constexpr std::size_t kKeyMax = 16;
// Bound on the orphan-skip probe (a crash leaves at most a handful of orphans).
constexpr std::uint64_t kMaxSeqProbe = 1u << 16;

[[nodiscard]] bool key_ok(std::string_view key) noexcept {
  if (key.empty() || key.size() > kKeyMax || key == "." || key == "..") return false;
  return std::all_of(key.begin(), key.end(), [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '.' || c == '-';
  });
}

// The NUL-terminated prefix of a fixed-capacity name; "" when it has no terminator
// (a full-width name is refused: every writer leaves room for one).
template <std::size_t N>
[[nodiscard]] std::string_view fixed_name(const std::array<char, N> &a) noexcept {
  const auto end = std::find(a.begin(), a.end(), '\0');
  if (end == a.end()) return {};
  // Everything after the terminator must be NUL too (zero-filled padding).
  if (std::any_of(end, a.end(), [](char c) { return c != '\0'; })) return {};
  return std::string_view{a.data(), static_cast<std::size_t>(end - a.begin())};
}

template <std::size_t N> void put_name(std::array<char, N> &a, std::string_view s) noexcept {
  a.fill('\0');
  std::copy_n(s.begin(), std::min(s.size(), N - 1), a.begin());
}

[[nodiscard]] PersistOutcome check_entries(std::span<const PersistManifestEntry> entries) {
  using S = PersistStatus;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const PersistManifestEntry &e = entries[i];
    const std::string_view key = fixed_name(e.key);
    const std::string_view name = fixed_name(e.file_name);
    if (!key_ok(key) || name.empty()) {
      return persist_error(S::RecordInvalid, kPersistManifestEntriesSection, i);
    }
    if (e.kind < static_cast<std::uint32_t>(PersistKind::MarketState) ||
        e.kind > static_cast<std::uint32_t>(PersistKind::RiskRun)) {
      return persist_error(S::RecordInvalid, kPersistManifestEntriesSection, i);
    }
    // The name is where an entry becomes a path: it must be exactly the canonical name,
    // so it can never carry a directory part.
    auto canonical = persist_file_name(static_cast<PersistKind>(e.kind), key, e.seq);
    if (!canonical || *canonical != name || e.seq == 0) {
      return persist_error(S::RecordInvalid, kPersistManifestEntriesSection, i);
    }
    if (i > 0 && !(fixed_name(entries[i - 1].key) < key)) {
      return persist_error(S::IndexUnsorted, kPersistManifestEntriesSection, i);
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (entries[j].seq == e.seq) {
        return persist_error(S::DuplicateKey, kPersistManifestEntriesSection, i);
      }
    }
  }
  return {};
}

[[nodiscard]] PersistResult<std::vector<std::byte>>
manifest_image(std::span<const PersistManifestEntry> entries) {
  const PersistSectionInput in{kPersistManifestEntriesSection, 1, 0, PersistDType::Pod,
                               static_cast<std::uint32_t>(sizeof(PersistManifestEntry)), true,
                               PersistCodec::None, PersistFilter::None, 3,
                               std::as_bytes(entries)};
  PersistFileInfo info;
  info.kind = PersistKind::Manifest;
  info.schema_fingerprint = kManifestFingerprint;
  if (!entries.empty()) {
    info.ts_min_ns = entries.front().ts_min_ns;
    info.ts_max_ns = entries.front().ts_max_ns;
    for (const PersistManifestEntry &e : entries) {
      info.ts_min_ns = std::min(info.ts_min_ns, e.ts_min_ns);
      info.ts_max_ns = std::max(info.ts_max_ns, e.ts_max_ns);
    }
  }
  return build_persist_file(std::span{&in, 1}, info);
}

// The header of a complete image, validated at Framing level as the kind it claims.
[[nodiscard]] PersistResult<PersistFile> check_image(std::span<const std::byte> image) {
  if (image.size() < sizeof(PersistHeader)) return persist_error(PersistStatus::Truncated);
  PersistHeader h{};
  std::memcpy(&h, image.data(), sizeof h);
  const auto kind = static_cast<PersistKind>(h.kind);
  if (h.kind < static_cast<std::uint32_t>(PersistKind::MarketState) ||
      h.kind > static_cast<std::uint32_t>(PersistKind::RiskRun)) {
    return persist_error(PersistStatus::InvalidArgument, kPersistNone, h.kind);
  }
  // SAFETY: address arithmetic only, to decide whether the caller's span can be
  // validated in place (open_bytes needs 8-byte alignment).
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  if (reinterpret_cast<std::uintptr_t>(image.data()) % 8u == 0u) {
    return PersistFile::open_bytes(image, nullptr, kind, h.schema_fingerprint,
                                   PersistVerify::Framing);
  }
  auto copy = std::make_shared<std::vector<std::uint64_t>>((image.size() + 7u) / 8u);
  std::memcpy(copy->data(), image.data(), image.size());
  const auto bytes = std::as_bytes(std::span<const std::uint64_t>{*copy}).first(image.size());
  return PersistFile::open_bytes(bytes, copy, kind, h.schema_fingerprint, PersistVerify::Framing);
}

[[nodiscard]] PersistResult<std::vector<PersistManifestEntry>> read_entries(const fs::path &dir) {
  auto d = PersistDirectory::open(dir.string());
  if (!d) {
    if (d.error().status == PersistStatus::NotFound) return std::vector<PersistManifestEntry>{};
    return tl::unexpected(d.error());
  }
  return std::vector<PersistManifestEntry>(d->entries().begin(), d->entries().end());
}

} // namespace

std::uint64_t persist_manifest_fingerprint() noexcept { return kManifestFingerprint; }

PersistResult<std::string> persist_file_name(PersistKind kind, std::string_view key,
                                             std::uint64_t seq) {
  if (!key_ok(key)) return persist_error(PersistStatus::InvalidArgument);
  std::array<char, 24> digits{};
  const int n = std::snprintf(digits.data(), digits.size(), "%012llu",
                              static_cast<unsigned long long>(seq));
  if (n <= 0) return persist_error(PersistStatus::InvalidArgument);
  const std::string_view seq12{digits.data(), static_cast<std::size_t>(n)};
  std::string name;
  switch (kind) {
  case PersistKind::Manifest: return persist_error(PersistStatus::InvalidArgument);
  case PersistKind::MarketState:
    name = std::string("bucket_et=") + std::string(key) + "-g" + std::string(seq12) + ".atxms";
    break;
  case PersistKind::SurfaceTimeline:
    name = std::string(key) + "-g" + std::string(seq12) + ".atxstl";
    break;
  case PersistKind::RiskRun: name = "seg-" + std::string(seq12) + ".atxrr"; break;
  }
  if (name.empty() || name.size() >= std::tuple_size_v<decltype(PersistManifestEntry::file_name)>) {
    return persist_error(PersistStatus::InvalidArgument);
  }
  return name;
}

PersistResult<PersistDirectory> PersistDirectory::open(std::string_view dir) {
  const fs::path root{std::string(dir)};
  const fs::path manifest = root / std::string(kPersistManifestFileName);
  std::error_code ec;
  if (!fs::is_directory(root, ec) || !fs::is_regular_file(manifest, ec)) {
    return persist_error(PersistStatus::NotFound);
  }
  // Read, never map: the manifest is replaced on every publish.
  auto file = PersistFile::open(manifest.string(), PersistKind::Manifest, kManifestFingerprint,
                                PersistOpenOptions{false, PersistVerify::Paranoid, false, 0});
  if (!file) return tl::unexpected(file.error());
  if (PersistOutcome ok = file->check_schema(kManifestSchema); !ok) {
    return tl::unexpected(ok.error());
  }
  auto rows = file->typed<PersistManifestEntry>(kPersistManifestEntriesSection);
  if (!rows) return tl::unexpected(rows.error());
  if (PersistOutcome ok = check_entries(*rows); !ok) return tl::unexpected(ok.error());
  PersistDirectory d;
  d.dir_ = root.string();
  d.entries_.assign(rows->begin(), rows->end());
  return d;
}

const PersistManifestEntry *PersistDirectory::find(std::string_view key) const noexcept {
  const auto it = std::lower_bound(entries_.begin(), entries_.end(), key,
                                   [](const PersistManifestEntry &e, std::string_view k) {
                                     return fixed_name(e.key) < k;
                                   });
  return it == entries_.end() || fixed_name(it->key) != key ? nullptr : &*it;
}

std::string PersistDirectory::path(const PersistManifestEntry &entry) const {
  return (fs::path{dir_} / std::string(fixed_name(entry.file_name))).string();
}

PersistResult<PersistManifestEntry> PersistDirectory::publish(std::string_view dir,
                                                              std::string_view key,
                                                              std::span<const std::byte> image,
                                                              std::uint32_t records) {
  if (!key_ok(key)) return persist_error(PersistStatus::InvalidArgument);
  auto file = check_image(image);
  if (!file) return tl::unexpected(file.error());
  const PersistHeader &h = file->header();
  const fs::path root{std::string(dir)};
  std::error_code ec;
  fs::create_directories(root, ec);
  if (!fs::is_directory(root, ec)) return persist_error(PersistStatus::IoError);

  auto lock = detail::WriterLock::acquire((root / std::string(kPersistLockFileName)).string());
  if (!lock) {
    const bool busy = lock.error().code() == ErrorCode::Unavailable;
    return persist_error(busy ? PersistStatus::Busy : PersistStatus::IoError);
  }
  auto entries = read_entries(root);
  if (!entries) return tl::unexpected(entries.error());
  std::uint64_t seq = 1;
  for (const PersistManifestEntry &e : *entries) seq = std::max(seq, e.seq + 1);
  // Skip names an earlier crash left behind (orphans are never overwritten).
  std::string name;
  for (std::uint64_t probe = 0; probe < kMaxSeqProbe; ++probe, ++seq) {
    auto candidate = persist_file_name(static_cast<PersistKind>(h.kind), key, seq);
    if (!candidate) return tl::unexpected(candidate.error());
    if (!fs::exists(root / *candidate, ec)) {
      name = std::move(*candidate);
      break;
    }
  }
  if (name.empty()) return persist_error(PersistStatus::IoError);
  if (PersistOutcome ok = publish_persist_file((root / name).string(), image); !ok) {
    return tl::unexpected(ok.error());
  }

  PersistManifestEntry e{};
  e.seq = seq;
  put_name(e.file_name, name);
  put_name(e.key, key);
  e.file_size = h.file_size;
  e.file_crc32c = file->footer().file_crc32c;
  e.header_crc32c = h.header_crc32c;
  e.kind = h.kind;
  e.records = records;
  e.schema_fingerprint = h.schema_fingerprint;
  e.source_fingerprint = h.source_fingerprint;
  e.ts_min_ns = h.ts_min_ns;
  e.ts_max_ns = h.ts_max_ns;
  std::vector<PersistManifestEntry> next = std::move(*entries);
  const auto at = std::lower_bound(next.begin(), next.end(), key,
                                   [](const PersistManifestEntry &x, std::string_view k) {
                                     return fixed_name(x.key) < k;
                                   });
  if (at != next.end() && fixed_name(at->key) == key) {
    *at = e;
  } else {
    next.insert(at, e);
  }
  auto manifest = manifest_image(next);
  if (!manifest) return tl::unexpected(manifest.error());
  if (PersistOutcome ok = persist_detail::replace_file(
          (root / std::string(kPersistManifestFileName)).string(), *manifest);
      !ok) {
    return tl::unexpected(ok.error());
  }
  return e;
}

} // namespace atx::vol
