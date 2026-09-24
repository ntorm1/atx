#pragma once

#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "panel_artifact.hpp"

namespace atx::impl {

struct SourceFileDigest {
    std::string filename;
    std::string sha256;
    atx::u64 size_bytes{};
};

struct IngestionInput {
    SourceFileDigest input;
    std::string preparation_sha256;
    std::string original_source_sha256;
    std::string preparation_policy;
    atx::u64 prepared_rows{};
};

struct PanelSourceProvenance {
    std::vector<PanelParent> parents;
    bool ingestion_verified{};
    bool preparation_verified{};
};

// Cold-path provenance helpers. Hashes establish content bindings, not authenticity
// or historical publication/revision times. JSON reads are bounded to 16 MiB.
// begin requires an absent/empty output directory and reserves it with a pending
// marker. Failed runs intentionally leave an unverified directory for inspection.
[[nodiscard]] atx::core::Result<IngestionInput>
begin_ingestion_provenance(const std::string &zip, const std::string &out_dir,
                           const std::string &preparation_manifest);

// Verify source/preparation bytes did not change, hash all generated segments, and
// publish _ingestion.manifest.json last. Returns its SHA-256. recipe is deterministic
// text containing resolved loader defaults and row counts; paths are excluded.
[[nodiscard]] atx::core::Result<std::string>
finish_ingestion_provenance(const IngestionInput &input, const std::string &zip,
                            const std::string &out_dir,
                            const std::string &preparation_manifest,
                            const std::string &recipe, atx::i64 dates_written,
                            atx::i64 rows_read);

// Snapshot selected .seg files before assembly. The returned order is lexical by
// filename, matching indexed attachment. Only segments overlapping [start,end) are
// hashed. validate checks the actual engine-selected paths and rehashes those files.
[[nodiscard]] atx::core::Result<std::vector<SourceFileDigest>>
snapshot_panel_sources(const std::string &seg_dir, atx::i64 start, atx::i64 end);

// Every selected segment gets a parent hash. An existing ingestion receipt must
// validate; absence records unknown upstream provenance. An explicit preparation
// manifest requires a matching ingestion binding and cannot establish one alone.
[[nodiscard]] atx::core::Result<PanelSourceProvenance>
validate_panel_sources(const std::string &seg_dir,
                        std::span<const SourceFileDigest> before,
                        std::span<const std::string> actual_paths,
                        const std::string &preparation_manifest);

// Hash the running executable once per process on Windows/Linux; empty on other
// platforms. Filesystem/hash errors are surfaced, not replaced by fabricated IDs.
[[nodiscard]] atx::core::Result<std::string> current_executable_sha256();

} // namespace atx::impl
