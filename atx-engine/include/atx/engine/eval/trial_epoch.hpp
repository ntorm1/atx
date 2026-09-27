#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::eval {

// E2 declarations are not P&L observations. This catalog cannot supply cluster-N,
// DSR moments, or evidence that unrelated fixed-calendar registries reconcile.
struct TrialEpochLimits {
    usize max_file_bytes{64U * 1024U * 1024U};
    usize max_record_bytes{4U * 1024U * 1024U};
    usize max_working_bytes{256U * 1024U * 1024U};
    usize max_attempts{4096};
    usize max_declared_cells{65536};
};
struct TrialEpochLineage {
    std::string prereg_sha256;
    std::string family_sha256;
    std::string trial_id;
};
struct TrialEpochCell {
    std::string canonical_dsl;
    int sign{1};
    usize horizon{};
    // Existing IC bootstrap streams depend on these numerical indices. They
    // are not display aliases and must remain part of the cell identity.
    usize stream_signal_index{};
    usize stream_horizon_index{};
    std::string forward_variant;
    std::string restriction;
    std::string family_sha256;
    std::string display_alias;
    std::optional<TrialEpochLineage> retained{};
};
struct TrialEpochAttempt {
    std::string token; // lowercase SHA256 of the caller's stable execution identity
    std::string trial_id;
    std::string prereg_sha256; // exact preregistration file bytes
    // Canonical JSON from an explicit numerical/data/window whitelist. No display
    // labels, checkpoint, output paths, or documentation-only producer revision.
    std::string numerical_recipe_json;
    std::vector<TrialEpochCell> cells;
};
struct TrialEpochCounts {
    usize unique_cells{};
    usize declared_cells{}; // measured declarations, including exact repeats
    usize verified_retained_cells{};
    usize attempts{};
    usize completed{};
    usize failed{};
    usize incomplete{};
};
struct TrialEpochReservation {
    bool inserted{};
    usize new_unique_cells{};
    usize verified_retained_cells{};
    std::string reservation_head_sha256;
    std::string numerical_recipe_sha256;
    TrialEpochCounts counts;
};
enum class TrialEpochTerminal : u8 { Completed = 1, Failed = 2 };

// Single-threaded handle. Every operation takes a fail-fast exclusive OS file
// lock, validates the complete SHA chain and exact external head, then appends a
// flushed reservation/terminal frame. A durable pending reservation counts even
// after process death. Torn records fail closed; no automatic repair/truncation.
// Reopen needs the current external head (64 zeros only for an empty new file).
// Exact repeated tokens are metadata-idempotent; callers must NOT rerun their VM.
// Bind immutable run manifests to reservation_head, then bind their hash in the
// terminal event. Publish the refreshed external anchor separately (no hash cycle).
class TrialEpochCatalog {
public:
    TrialEpochCatalog(TrialEpochCatalog &&) noexcept;
    TrialEpochCatalog &operator=(TrialEpochCatalog &&) noexcept;
    ~TrialEpochCatalog();
    TrialEpochCatalog(const TrialEpochCatalog &) = delete;
    TrialEpochCatalog &operator=(const TrialEpochCatalog &) = delete;

    [[nodiscard]] static core::Result<TrialEpochCatalog> open(
        const std::string &path, const std::string &epoch,
        const std::string &expected_head_sha256, TrialEpochLimits limits = {});
    [[nodiscard]] core::Result<TrialEpochReservation> reserve_attempt(const TrialEpochAttempt &);
    [[nodiscard]] core::Status finish_attempt(const std::string &token,
        TrialEpochTerminal status, const std::string &result_sha256);
    [[nodiscard]] TrialEpochCounts counts() const noexcept;
    [[nodiscard]] const std::string &head_sha256() const noexcept;
private:
    struct Impl;
    explicit TrialEpochCatalog(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
} // namespace atx::engine::eval
