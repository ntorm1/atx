#include "panel_pipeline.hpp"

#include <array>
#include <filesystem>
#include <system_error>
#include <utility>

#include "serialize_panel.hpp"
#include "atx/core/sha256.hpp"

namespace atx::impl {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

atx::core::Status require_matching_identity_status(const PipelinePanel& left,
                                                  const PipelinePanel& right) {
    if (left.identity.has_value() != right.identity.has_value()) {
        return Err(ErrorCode::InvalidArgument,
                   "panel join: identified and unidentified inputs cannot be mixed");
    }
    return Ok();
}

} // namespace

PipelineOutputGuard::~PipelineOutputGuard() {
    std::error_code ec;
    std::filesystem::remove(lock_path_, ec);
    // Destructors cannot report cleanup failure. A leftover empty lock prevents
    // another publication and is inspectable, rather than permitting overlap.
}

atx::core::Result<std::unique_ptr<PipelineOutputGuard>>
reserve_pipeline_output(const std::string& path, bool identified) {
    const auto parent = std::filesystem::path(path).parent_path();
    std::error_code ec;
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return Err(ErrorCode::IoError, "cannot create panel output directory: " + ec.message());
        }
    }
    const std::string lock_path = path + ".stage-lock";
    if (!std::filesystem::create_directory(lock_path, ec) || ec) {
        return Err(ErrorCode::IoError, "panel output is reserved or inaccessible: " + path);
    }
    auto guard = std::make_unique<PipelineOutputGuard>(lock_path);
    constexpr std::array<const char*, 3> pending_suffixes{
        ".partial", ".manifest.json.partial", ".publishing"};
    for (const auto* suffix : pending_suffixes) {
        if (std::filesystem::exists(path + suffix, ec) || ec) {
            return Err(ErrorCode::InvalidArgument,
                       "panel output has an incomplete publication: " + path);
        }
    }
    const bool manifest_exists = std::filesystem::exists(path + ".manifest.json", ec);
    if (ec || manifest_exists) {
        return Err(ErrorCode::InvalidArgument, "panel output manifest already exists: " + path);
    }
    if (identified && (std::filesystem::exists(path, ec) || ec)) {
        return Err(ErrorCode::InvalidArgument, "identified output requires a fresh panel path: " + path);
    }
    return Ok(std::move(guard));
}

atx::core::Result<PipelinePanel>
read_pipeline_panel(const std::string& path, bool allow_unidentified) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        return Err(ErrorCode::InvalidArgument,
                   "panel store V2 requires explicit bounded window and separate tradable-mask consumer");
    }
    if (ec) return Err(ErrorCode::IoError, "panel: cannot inspect input path");
    const bool present = std::filesystem::exists(path + ".manifest.json", ec);
    if (ec) {
        return Err(ErrorCode::IoError, "panel: cannot inspect identity manifest: " + ec.message());
    }
    if (present || !allow_unidentified) {
        ATX_TRY(auto artifact, read_panel_artifact(path));
        return Ok(PipelinePanel{std::move(artifact.panel), std::move(artifact.identity),
                                std::move(artifact.artifact_id)});
    }
    ATX_TRY(auto panel, read_panel(path));
    return Ok(PipelinePanel{std::move(panel), std::nullopt, {}});
}

atx::core::Result<PipelinePanel> read_pipeline_store_window(const std::string& directory,
    atx::usize begin, atx::usize end, atx::u64 budget, std::string_view expected) {
    ATX_TRY(auto window, read_panel_store_window(directory, begin, end, budget, expected));
    return Ok(PipelinePanel{std::move(window.artifact.panel), std::move(window.artifact.identity),
        std::move(window.artifact.artifact_id), std::move(window.tradable), std::move(window.fields)});
}

atx::core::Status require_pipeline_parent(const PipelinePanel& child,
                                         const PipelinePanel& parent,
                                         const std::string& role) {
    ATX_TRY_VOID(require_matching_identity_status(child, parent));
    if (!child.identity) {
        return Ok();
    }
    ATX_TRY_VOID(require_same_panel_axes(*child.identity, *parent.identity));
    return require_panel_parent(*child.identity, role, parent.artifact_id);
}

atx::core::Status require_book_schedule(const PipelinePanel& books,
                                       const PipelinePanel& research,
                                       std::span<const atx::usize> periods) {
    ATX_TRY_VOID(require_matching_identity_status(books, research));
    if (periods.size() != books.panel.dates() ||
        books.panel.instruments() != research.panel.instruments()) {
        return Err(ErrorCode::InvalidArgument, "report: book schedule shape mismatch");
    }
    for (atx::usize i = 0; i < periods.size(); ++i) {
        if (periods[i] >= research.panel.dates() || (i > 0 && periods[i] <= periods[i - 1])) {
            return Err(ErrorCode::InvalidArgument, "report: invalid or unordered book schedule");
        }
    }
    if (!books.identity) {
        return Ok();
    }
    ATX_TRY_VOID(require_panel_parent(*books.identity, "research", research.artifact_id));
    if (books.identity->instrument_namespace != research.identity->instrument_namespace ||
        books.identity->instrument_ids != research.identity->instrument_ids ||
        books.identity->original_instrument_indices !=
            research.identity->original_instrument_indices) {
        return Err(ErrorCode::InvalidArgument, "report: book security identities differ");
    }
    for (atx::usize i = 0; i < periods.size(); ++i) {
        if (books.identity->session_keys[i] != research.identity->session_keys[periods[i]]) {
            return Err(ErrorCode::InvalidArgument, "report: book dates differ from bound schedule");
        }
    }
    return Ok();
}

atx::core::Status require_pipeline_file(const PipelinePanel& artifact,
                                       const std::string& role, const std::string& path) {
    if (!artifact.identity) {
        return Ok();
    }
    ATX_TRY(auto hash, atx::core::sha256_file(path));
    return require_panel_parent(*artifact.identity, role, hash);
}

atx::core::Result<atx::u64>
write_pipeline_panel(const atx::engine::alpha::Panel& panel, const std::string& path,
                     const PipelinePanel& research, std::span<const atx::usize> selected_rows,
                     std::string recipe, std::vector<PanelParent> other_parents) {
    if (!research.tradable.empty())
        return Err(ErrorCode::InvalidArgument,
                   "V2 research requires explicit separate-mask publication; legacy writer cannot drop tradability");
    if (!research.identity) {
        std::error_code ec;
        const bool has_manifest = std::filesystem::exists(path + ".manifest.json", ec);
        if (ec || has_manifest) {
            return Err(ErrorCode::InvalidArgument,
                       "legacy output cannot overwrite an identified artifact");
        }
        return write_panel(panel, path);
    }
    PanelIdentity identity = *research.identity;
    if (!selected_rows.empty()) {
        identity.session_keys.clear();
        identity.session_keys.reserve(selected_rows.size());
        for (const auto index : selected_rows) {
            if (index >= research.identity->session_keys.size()) {
                return Err(ErrorCode::InvalidArgument, "derived panel date outside parent axis");
            }
            identity.session_keys.push_back(research.identity->session_keys[index]);
        }
    }
    identity.recipe = std::move(recipe);
    identity.parents = std::move(other_parents);
    identity.parents.push_back(PanelParent{"research", research.artifact_id});
    ATX_TRY(auto written, write_panel_artifact(panel, path, identity));
    return Ok(written.payload_digest);
}

} // namespace atx::impl
