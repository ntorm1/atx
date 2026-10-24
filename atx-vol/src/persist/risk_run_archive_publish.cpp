// risk_run_archive_publish.cpp — publication of RiskRun segments and manifests at a
// selectable durability, with the single-writer check (lane P2 fix rounds,
// 2026-10-24; SealDurability in risk_run_archive.hpp, the writer API in
// risk_run_archive_publish.hpp).
//
// PROTOCOL. PersistDirectory's (persist_file.hpp): under `dir/persist.lock` read the
// manifest, write the data file under a fresh canonical name (an earlier crash's
// orphan is never overwritten), then replace the manifest. Every file goes through a
// unique temp beside its target and a rename, so a reader never observes a partial
// file, and a crash between the two leaves an unreferenced orphan, never a dangling
// entry. The durability differs:
//   Flush    temp, FlushFileBuffers, rename (detail::flush_and_publish_file), for the
//            segment and the manifest: what PersistDirectory::publish does.
//   Publish  temp, rename: no flush. The OS cache holds the bytes, so a process crash
//            loses nothing sealed; a power loss before write-back can leave a torn
//            file, which RiskRunArchive::open refuses by CRC.
//
// OWNERSHIP. A writer's seal is refused (SegmentMismatch) unless the manifest still
// ends with the row that writer published last — see risk_run_archive_publish.hpp.
// Compaction compares the whole manifest with the one it read (publish_compacted).
//
// WHY HERE. PersistDirectory::publish always flushes, replaces a same-key row and can
// add or replace one key only; compaction must drop every key but one.
// PersistDirectory::replace_all was deferred (ruling 2026-10-24), so the manifest image
// is built here, byte-compatible with PersistDirectory's (manifest kind + fingerprint,
// one Pod section of PersistManifestEntry rows sorted by key, header ts range = the
// union of the rows'). Only the M1 public API of the container is used
// (build_persist_file, persist_file_name, PersistDirectory::open) plus atx-vol's
// storage primitives.

#include "persist/risk_run_archive_publish.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/api/persist/persist_file.hpp"
#include "atx/vol/api/persist/persist_format.hpp"
#include "persist/risk_run_format.hpp"
#include "storage/archive_util.hpp"
#include "storage/writer_lock.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace atx::vol::risk_run {
namespace {

namespace fs = std::filesystem;
using S = PersistStatus;

template <std::size_t N> void put_name(std::array<char, N> &a, std::string_view s) noexcept {
  a.fill('\0');
  std::copy_n(s.begin(), std::min(s.size(), N - 1), a.begin());
}

template <std::size_t N>
[[nodiscard]] std::string_view fixed_name(const std::array<char, N> &a) noexcept {
  const auto end = std::find(a.begin(), a.end(), '\0');
  return {a.data(), static_cast<std::size_t>(end - a.begin())};
}

// Rename `from` onto `to` (replacing it), retrying with a doubling backoff (~1 s in
// all) while a reader holds `to` open without FILE_SHARE_DELETE: the window
// detail::flush_and_publish_file covers on the Flush path.
[[nodiscard]] bool rename_with_retry(const fs::path &from, const fs::path &to) {
  std::chrono::milliseconds wait{1};
  for (int attempt = 0; attempt < 11; ++attempt) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec) return true;
    std::this_thread::sleep_for(wait);
    wait *= 2;
  }
  return false;
}

// Write `bytes` to `path` through a unique temp beside it, then rename it into place;
// the temp is flushed first when `durability` is Flush. On failure nothing is at
// `path` (a Publish temp is removed; flush_and_publish_file keeps its temp for
// recovery, as it documents).
[[nodiscard]] PersistOutcome write_file(const fs::path &path, std::span<const std::byte> bytes,
                                        SealDurability durability) {
  auto tmp = atx::vol::detail::reserve_unique_publish_temp_file(path.string());
  if (!tmp) return persist_error(S::IoError);
  const fs::path tmp_path{*tmp};
  std::error_code ignored;
  {
    std::ofstream os(tmp_path, std::ios::binary | std::ios::trunc);
    if (os) {
      // SAFETY: std::byte -> char view for ostream::write; char may alias any object.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      os.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
      os.close();
    }
    if (os.fail()) {
      fs::remove(tmp_path, ignored);
      return persist_error(S::IoError);
    }
  }
  if (durability == SealDurability::Flush) {
    if (!atx::vol::detail::flush_and_publish_file(tmp_path.string(), path.string())) {
      return persist_error(S::IoError);
    }
    return {};
  }
  if (!rename_with_retry(tmp_path, path)) {
    fs::remove(tmp_path, ignored);
    return persist_error(S::IoError);
  }
  return {};
}

// The manifest image of `entries` (sorted by key): PersistDirectory's layout.
[[nodiscard]] PersistResult<std::vector<std::byte>>
manifest_image(std::span<const PersistManifestEntry> entries) {
  const PersistSectionInput in{kPersistManifestEntriesSection, 1, 0, PersistDType::Pod,
                               static_cast<std::uint32_t>(sizeof(PersistManifestEntry)), true,
                               PersistCodec::None, PersistFilter::None, 3,
                               std::as_bytes(entries)};
  PersistFileInfo info;
  info.kind = PersistKind::Manifest;
  info.schema_fingerprint = persist_manifest_fingerprint();
  if (!entries.empty()) {
    info.ts_min_ns = std::numeric_limits<std::int64_t>::max();
    info.ts_max_ns = std::numeric_limits<std::int64_t>::min();
    for (const PersistManifestEntry &e : entries) {
      info.ts_min_ns = std::min(info.ts_min_ns, e.ts_min_ns);
      info.ts_max_ns = std::max(info.ts_max_ns, e.ts_max_ns);
    }
  }
  return build_persist_file(std::span{&in, 1}, info);
}

[[nodiscard]] PersistOutcome write_manifest(const fs::path &root,
                                            std::span<const PersistManifestEntry> entries,
                                            SealDurability durability) {
  auto image = manifest_image(entries);
  if (!image) return tl::unexpected(image.error());
  return write_file(root / std::string(kPersistManifestFileName), *image, durability);
}

// The manifest row of `image` (a complete PersistFile built in this process) published
// as `name` for `key` at `seq`.
[[nodiscard]] PersistManifestEntry entry_of(std::span<const std::byte> image,
                                            std::string_view name, std::string_view key,
                                            std::uint64_t seq, std::uint32_t records) noexcept {
  PersistHeader h{};
  PersistFooter ft{};
  std::memcpy(&h, image.data(), sizeof h);
  std::memcpy(&ft, image.data() + image.size() - sizeof ft, sizeof ft);
  PersistManifestEntry e{};
  e.seq = seq;
  put_name(e.file_name, name);
  put_name(e.key, key);
  e.file_size = h.file_size;
  e.file_crc32c = ft.file_crc32c;
  e.header_crc32c = h.header_crc32c;
  e.kind = h.kind;
  e.records = records;
  e.schema_fingerprint = h.schema_fingerprint;
  e.source_fingerprint = h.source_fingerprint;
  e.ts_min_ns = h.ts_min_ns;
  e.ts_max_ns = h.ts_max_ns;
  return e;
}

// The manifest's rows; empty when the run has no manifest yet.
[[nodiscard]] PersistResult<std::vector<PersistManifestEntry>> read_entries(std::string_view dir) {
  auto d = PersistDirectory::open(dir);
  if (!d) {
    if (d.error().status == S::NotFound) return std::vector<PersistManifestEntry>{};
    return tl::unexpected(d.error());
  }
  return std::vector<PersistManifestEntry>(d->entries().begin(), d->entries().end());
}

// The first canonical file name of `key` at a seq >= `seq` that is not on disk (an
// earlier crash's orphan is never overwritten); `seq` advances to the chosen one.
[[nodiscard]] PersistResult<std::string> fresh_name(const fs::path &root, std::string_view key,
                                                    std::uint64_t &seq) {
  std::error_code ec;
  for (std::uint32_t probe = 0; probe < (1u << 16); ++probe, ++seq) {
    auto candidate = persist_file_name(PersistKind::RiskRun, key, seq);
    if (!candidate) return tl::unexpected(candidate.error());
    if (!fs::exists(root / *candidate, ec)) return candidate;
  }
  return persist_error(S::IoError);
}

[[nodiscard]] PersistResult<atx::vol::detail::WriterLock> lock_dir(const fs::path &root) {
  auto lock = atx::vol::detail::WriterLock::acquire(
      (root / std::string(kPersistLockFileName)).string());
  if (!lock) {
    const bool busy = lock.error().code() == atx::core::ErrorCode::Unavailable;
    return persist_error(busy ? S::Busy : S::IoError);
  }
  return std::move(*lock);
}

[[nodiscard]] bool durability_ok(SealDurability d) noexcept {
  return d == SealDurability::Publish || d == SealDurability::Flush;
}

// The writer still owns the run: the manifest is empty (its first segment) or its
// newest row is the one the writer published last.
[[nodiscard]] bool owns(std::span<const PersistManifestEntry> entries,
                        const std::optional<WriterTail> &tail) noexcept {
  if (!tail) return entries.empty();
  const auto newest =
      std::max_element(entries.begin(), entries.end(),
                       [](const PersistManifestEntry &a, const PersistManifestEntry &b) {
                         return a.seq < b.seq;
                       });
  return newest != entries.end() && newest->seq == tail->seq &&
         newest->file_crc32c == tail->file_crc32c;
}

// FlushFileBuffers / fsync on an existing, fully written file.
[[nodiscard]] PersistOutcome flush_file(const std::string &path) {
#if defined(_WIN32)
  const std::wstring wide = fs::path{path}.wstring();
  HANDLE h = ::CreateFileW(wide.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    const DWORD err = ::GetLastError();
    return persist_error(err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION
                             ? S::Busy
                             : S::IoError);
  }
  const BOOL flushed = ::FlushFileBuffers(h);
  ::CloseHandle(h);
  if (!flushed) return persist_error(S::IoError);
  return {};
#else
  const int fd = ::open(path.c_str(), O_WRONLY);
  if (fd < 0) return persist_error(S::IoError);
  const int rc = ::fsync(fd);
  ::close(fd);
  if (rc != 0) return persist_error(S::IoError);
  return {};
#endif
}

} // namespace

PersistOutcome claim_run_dir(std::string_view dir) {
  if (dir.empty()) return persist_error(S::InvalidArgument);
  const fs::path root{std::string(dir)};
  std::error_code ec;
  fs::create_directories(root, ec);
  if (!fs::is_directory(root, ec)) return persist_error(S::IoError);
  auto lock = lock_dir(root);
  if (!lock) return tl::unexpected(lock.error());
  auto d = PersistDirectory::open(dir);
  if (d) return persist_error(S::AlreadyExists);
  if (d.error().status != S::NotFound) return tl::unexpected(d.error());
  return {};
}

PersistResult<PersistManifestEntry>
publish_writer_segment(std::string_view dir, std::string_view key,
                       std::span<const std::byte> image, std::uint32_t records,
                       SealDurability durability, const std::optional<WriterTail> &tail) {
  if (!durability_ok(durability) || image.size() < sizeof(PersistHeader) + sizeof(PersistFooter)) {
    return persist_error(S::InvalidArgument);
  }
  const fs::path root{std::string(dir)};
  std::error_code ec;
  fs::create_directories(root, ec);
  if (!fs::is_directory(root, ec)) return persist_error(S::IoError);
  auto lock = lock_dir(root);
  if (!lock) return tl::unexpected(lock.error());
  auto entries = read_entries(dir);
  if (!entries) return tl::unexpected(entries.error());
  if (!owns(*entries, tail)) {
    return persist_error(S::SegmentMismatch, kPersistManifestEntriesSection,
                         tail ? tail->seq : 0u);
  }
  std::vector<PersistManifestEntry> &next = *entries;
  const auto at = std::lower_bound(next.begin(), next.end(), key,
                                   [](const PersistManifestEntry &x, std::string_view k) {
                                     return fixed_name(x.key) < k;
                                   });
  if (at != next.end() && fixed_name(at->key) == key) {
    return persist_error(S::DuplicateKey, kPersistManifestEntriesSection, at->seq);
  }
  std::uint64_t seq = 1;
  for (const PersistManifestEntry &e : next) seq = std::max(seq, e.seq + 1);
  auto name = fresh_name(root, key, seq);
  if (!name) return tl::unexpected(name.error());
  if (PersistOutcome ok = write_file(root / *name, image, durability); !ok) {
    return tl::unexpected(ok.error());
  }
  const PersistManifestEntry e = entry_of(image, *name, key, seq, records);
  next.insert(at, e);
  if (PersistOutcome ok = write_manifest(root, next, durability); !ok) {
    return tl::unexpected(ok.error());
  }
  return e;
}

PersistOutcome publish_compacted(std::string_view dir, std::span<const PersistManifestEntry> seen,
                                 std::span<const std::byte> image, std::uint32_t records,
                                 SealDurability durability) {
  if (!durability_ok(durability) || image.size() < sizeof(PersistHeader) + sizeof(PersistFooter)) {
    return persist_error(S::InvalidArgument);
  }
  const fs::path root{std::string(dir)};
  auto lock = lock_dir(root);
  if (!lock) return tl::unexpected(lock.error());
  auto now = PersistDirectory::open(dir);
  if (!now) return tl::unexpected(now.error());
  const auto same = [](const PersistManifestEntry &a, const PersistManifestEntry &b) {
    return a.seq == b.seq && a.file_crc32c == b.file_crc32c;
  };
  if (now->entries().size() != seen.size() ||
      !std::equal(seen.begin(), seen.end(), now->entries().begin(), same)) {
    return persist_error(S::SegmentMismatch, kPersistManifestEntriesSection);
  }
  std::uint64_t seq = 1;
  for (const PersistManifestEntry &e : seen) seq = std::max(seq, e.seq + 1);
  const std::string key = segment_key(0);
  auto name = fresh_name(root, key, seq);
  if (!name) return tl::unexpected(name.error());
  if (PersistOutcome ok = write_file(root / *name, image, durability); !ok) return ok;
  const PersistManifestEntry e = entry_of(image, *name, key, seq, records);
  return write_manifest(root, std::span{&e, 1}, durability);
}

PersistOutcome flush_files(std::vector<std::string> &paths) {
  while (!paths.empty()) {
    if (PersistOutcome ok = flush_file(paths.back()); !ok) return ok;
    paths.pop_back();
  }
  return {};
}

} // namespace atx::vol::risk_run
