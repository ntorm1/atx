#pragma once

// risk_run_archive_publish.hpp — RiskRun publication with the single-writer check
// (lane P2 fix round 2, 2026-10-24). Implemented in risk_run_archive_publish.cpp.
//
// SINGLE WRITER. A run directory has one writer for its whole life, and the manifest
// is how a writer knows it still owns the run. claim_run_dir() refuses a directory that
// already holds a run, and every seal checks, under the directory's writer lock, that
// the manifest still ends with the row this writer published last (or is still absent,
// for its first segment). A second writer on the same directory, or a compaction that
// replaced the manifest under a live writer, makes the next seal fail loudly
// (SegmentMismatch) instead of splicing or orphaning segments.
//
// This header supersedes the 5-argument publish_segment declared in
// risk_run_format.hpp (that header is being re-pinned by lane E this round; the stale
// declaration is unused and undefined, to be dropped at integration).

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/api/persist/persist_file.hpp"
#include "atx/vol/api/persist/persist_format.hpp"
#include "atx/vol/api/persist/risk_run_archive.hpp"

namespace atx::vol::risk_run {

// The manifest row a writer published last: it must still be the manifest's newest row
// (highest seq) when the writer publishes again.
struct WriterTail {
  std::uint64_t seq{0};
  std::uint32_t file_crc32c{0};
};

// Create `dir` if needed, take its writer lock and check that it holds no run (no
// manifest): AlreadyExists otherwise; Busy when a live writer holds the lock past its
// timeout; IoError when the directory cannot be created.
[[nodiscard]] PersistOutcome claim_run_dir(std::string_view dir);

// Publish `image` (a complete RiskRun segment) as segment `key` of `dir` at
// `durability`, under the writer lock: refused with SegmentMismatch unless the
// manifest is absent / empty (`tail` empty: the writer's first segment) or its newest
// row equals `*tail`; DuplicateKey when `key` is already listed (an existing row is
// never replaced). Returns the published manifest row.
[[nodiscard]] PersistResult<PersistManifestEntry>
publish_writer_segment(std::string_view dir, std::string_view key,
                       std::span<const std::byte> image, std::uint32_t records,
                       SealDurability durability, const std::optional<WriterTail> &tail);

// FlushFileBuffers (POSIX: fsync) on each of `paths`, last first, popping each one
// that is now durable. Busy when one is open elsewhere without write sharing (a mapped
// reader) — the rest stay in `paths` for a retry; IoError on any other failure.
[[nodiscard]] PersistOutcome flush_files(std::vector<std::string> &paths);

} // namespace atx::vol::risk_run
