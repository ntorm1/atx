#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "panel_artifact.hpp"

namespace atx::impl {

// Unknown identity is confined to explicitly requested legacy diagnostics.
struct PipelinePanel {
    atx::engine::alpha::Panel panel;
    std::optional<PanelIdentity> identity;
    std::string artifact_id;
};

// Reserves all companion writes for one stage. Failed runs leave diagnostic
// files; a published identified payload is never overwritten.
class PipelineOutputGuard {
public:
    explicit PipelineOutputGuard(std::string lock_path) : lock_path_(std::move(lock_path)) {}
    ~PipelineOutputGuard();
    PipelineOutputGuard(const PipelineOutputGuard&) = delete;
    PipelineOutputGuard& operator=(const PipelineOutputGuard&) = delete;
private:
    std::string lock_path_;
};

[[nodiscard]] atx::core::Result<std::unique_ptr<PipelineOutputGuard>>
reserve_pipeline_output(const std::string& path, bool identified);

[[nodiscard]] atx::core::Result<PipelinePanel>
read_pipeline_panel(const std::string& path, bool allow_unidentified);

// A positional combination must have exactly the parent's axes and derivation.
[[nodiscard]] atx::core::Status
require_pipeline_parent(const PipelinePanel& child, const PipelinePanel& parent,
                        const std::string& role);

[[nodiscard]] atx::core::Status
require_book_schedule(const PipelinePanel& books, const PipelinePanel& research,
                      std::span<const atx::usize> periods);

// Hash-bound companions are checked before any of their values influence a result.
[[nodiscard]] atx::core::Status
require_pipeline_file(const PipelinePanel& artifact, const std::string& role,
                      const std::string& path);

// Empty selected_rows means the complete parent's date axis. Publication is last,
// after callers have written and checked any companion files included in parents.
[[nodiscard]] atx::core::Result<atx::u64>
write_pipeline_panel(const atx::engine::alpha::Panel& panel, const std::string& path,
                     const PipelinePanel& research, std::span<const atx::usize> selected_rows,
                     std::string recipe, std::vector<PanelParent> other_parents = {});

} // namespace atx::impl
