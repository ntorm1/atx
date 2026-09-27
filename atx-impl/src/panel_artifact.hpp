#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/panel_store.hpp"

namespace atx::impl {

inline constexpr std::string_view kPanelSessionEncoding = "UnixNanoseconds";
inline constexpr std::string_view kPanelSessionSemantics = "session-label-not-availability";
inline constexpr std::string_view kSpiderRockSecurityIdNamespace = "spiderrock.securityID";

struct PanelParent {
    std::string role;
    std::string sha256;
};

struct PanelIdentity {
    std::string instrument_namespace;
    std::vector<atx::i64> session_keys;
    std::vector<std::string> instrument_ids;
    std::vector<atx::usize> original_instrument_indices;
    // Exact deterministic UTF-8 recipe bytes, not JSON canonicalization. Record
    // source-vintage/availability limitations here; the session keys imply neither.
    std::string recipe;
    std::vector<PanelParent> parents;
};

struct PanelArtifactReceipt {
    std::string artifact_id;
    std::string payload_sha256;
    atx::u64 payload_digest{}; // Existing APNL v1 FNV trailer / StageResult digest.
};

struct PanelArtifact {
    atx::engine::alpha::Panel panel;
    PanelIdentity identity;
    std::string artifact_id;
    std::string payload_sha256;
};

struct PanelStoreWindow {
    PanelArtifact artifact;
    std::vector<atx::u8> tradable; // decision-date mask, independent of Panel presence
    std::vector<atx::engine::data::PanelStoreField> fields;
};

// Explicit V2 adapter, bounded before full date x union allocation. The returned
// Panel uses SOURCE PRESENCE, retaining TS warm-up. Callers MUST carry tradable
// separately for cross-sectional/trading admission. `close` is loaded from the
// original-f64 channel, so existing close-ratio consumers don't reconstruct it
// from rounded f32 prices. Other f32 fields widen; exact return fields stay f64.
// This materializer is for a selected window, not the large-union open-RSS gate.
[[nodiscard]] atx::core::Result<PanelStoreWindow> read_panel_store_window(
    const std::string& directory, atx::usize begin, atx::usize end,
    atx::u64 max_materialized_bytes = 256ULL * 1024 * 1024,
    std::string_view expected_manifest_sha256 = {});

// Preserve the existing APNL v1 bytes and publish <path>.manifest.json last.
// Payload, manifest, partial files, and the publication-lock directory must be
// absent. Failures may leave partial files; use a fresh path after investigation.
// The manifest binds exact ordered axes, fields, recipe, and uniquely named parent
// SHA-256 values. Instrument IDs are unique canonical positive i64 decimal strings;
// session keys are strictly increasing i64 labels, serialized as decimal strings.
// Original instrument indices are required, unique, and preserved in output order.
// At least one named source/parent digest is required. Publication uses same-directory
// hard links for atomic no-replace behavior; unsupported filesystems fail closed.
// No claim of authentication, original publication times, or historical vintages.
[[nodiscard]] atx::core::Result<PanelArtifactReceipt>
write_panel_artifact(const atx::engine::alpha::Panel &panel, const std::string &path,
                     const PanelIdentity &identity);

// Strict identified read. Missing/invalid manifests, unsupported schema, malformed
// axes, or any component/payload hash mismatch fail; there is no legacy fallback.
// Validates APNL shape/field layout before calling the existing numeric reader.
[[nodiscard]] atx::core::Result<PanelArtifact>
read_panel_artifact(const std::string &path);

// Identical integrity checks, with an inclusive serialized APNL byte limit.
// Rejects both the manifest-declared payload size and the opened file's extent
// above max_payload_bytes before allocating the single captured payload buffer.
// Zero rejects every valid payload. This does not cap decoded storage, bounded
// manifest parsing, or total process memory; callers must budget those separately.
[[nodiscard]] atx::core::Result<PanelArtifact>
read_panel_artifact(const std::string &path, atx::u64 max_payload_bytes);

// Joins require ordered semantic axes AND the original-column mapping to agree.
// Validation also rejects empty/invalid identities; equal shapes are insufficient.
[[nodiscard]] atx::core::Status require_same_panel_axes(const PanelIdentity &left,
                                                       const PanelIdentity &right);

// Require the uniquely named parent binding to equal an externally known digest.
// A source-content digest or another panel's artifact_id can serve as the parent.
[[nodiscard]] atx::core::Status require_panel_parent(const PanelIdentity &child,
                                                    std::string_view role,
                                                    std::string_view parent_artifact_id);

} // namespace atx::impl
