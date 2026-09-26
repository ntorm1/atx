#include "panel_artifact.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "artifacts.hpp"
#include "serialize_panel.hpp"
#include "atx/core/sha256.hpp"

namespace atx::impl {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::Status;

constexpr atx::usize kMaxManifestBytes = 16U * 1024U * 1024U;
constexpr atx::usize kMaxRecipeBytes = 1024U * 1024U;
constexpr atx::usize kMaxAxisEntries = 1'000'000U;
constexpr atx::usize kMaxParents = 100'000U;
constexpr atx::usize kMaxFields = 4096U;
constexpr atx::usize kMaxLabelBytes = 1024U;
constexpr int kMaxJsonDepth = 16;
constexpr std::string_view kIdentityDomain = "atx-panel-artifact-v1\n";

[[nodiscard]] bool hash_valid(std::string_view hash, atx::usize length = 64) {
    return hash.size() == length && std::all_of(hash.begin(), hash.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

[[nodiscard]] Status text_valid(const std::string &text, atx::usize limit) {
    if (text.empty() || text.size() > limit || text.find('\0') != std::string::npos) {
        return Err(ErrorCode::InvalidArgument, "panel artifact: empty/oversized/NUL text");
    }
    // The existing JSON library validates UTF-8 when serializing a string.
    (void)Json(text).dump();
    return Ok();
}

template <typename Integer>
[[nodiscard]] Result<Integer> decimal(const std::string &text) {
    Integer value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        std::to_string(value) != text) {
        return Err(ErrorCode::ParseError, "panel artifact: noncanonical/out-of-range decimal");
    }
    return Ok(value);
}

[[nodiscard]] Status validate_identity(const PanelIdentity &identity) {
    ATX_TRY_VOID(text_valid(identity.instrument_namespace, kMaxLabelBytes));
    ATX_TRY_VOID(text_valid(identity.recipe, kMaxRecipeBytes));
    if (identity.session_keys.empty() || identity.session_keys.size() > kMaxAxisEntries ||
        identity.instrument_ids.empty() || identity.instrument_ids.size() > kMaxAxisEntries ||
        identity.original_instrument_indices.size() != identity.instrument_ids.size() ||
        identity.parents.empty() || identity.parents.size() > kMaxParents) {
        return Err(ErrorCode::InvalidArgument, "panel artifact: invalid axis/parent counts");
    }
    for (atx::usize i = 1; i < identity.session_keys.size(); ++i) {
        if (identity.session_keys[i] <= identity.session_keys[i - 1]) {
            return Err(ErrorCode::InvalidArgument, "panel artifact: sessions not increasing");
        }
    }
    std::set<std::string> ids;
    std::set<atx::usize> indices;
    for (atx::usize i = 0; i < identity.instrument_ids.size(); ++i) {
        const auto &id = identity.instrument_ids[i];
        ATX_TRY(auto value, decimal<atx::i64>(id));
        if (value <= 0 || !ids.insert(id).second ||
            !indices.insert(identity.original_instrument_indices[i]).second) {
            return Err(ErrorCode::InvalidArgument, "panel artifact: invalid/duplicate instrument");
        }
    }
    std::set<std::string> roles;
    for (const auto &parent : identity.parents) {
        ATX_TRY_VOID(text_valid(parent.role, kMaxLabelBytes));
        if (!hash_valid(parent.sha256) || !roles.insert(parent.role).second) {
            return Err(ErrorCode::InvalidArgument, "panel artifact: invalid/ambiguous parent");
        }
    }
    return Ok();
}

[[nodiscard]] Status validate_fields(const std::vector<std::string> &fields) {
    if (fields.empty() || fields.size() > kMaxFields) {
        return Err(ErrorCode::InvalidArgument, "panel artifact: invalid field count");
    }
    std::set<std::string> names;
    for (const auto &field : fields) {
        ATX_TRY_VOID(text_valid(field, kMaxLabelBytes));
        if (!names.insert(field).second) {
            return Err(ErrorCode::InvalidArgument, "panel artifact: duplicate field");
        }
    }
    return Ok();
}

[[nodiscard]] Json axes_json(const PanelIdentity &identity) {
    Json dates = Json::array();
    for (const auto value : identity.session_keys) dates.push_back(std::to_string(value));
    Json indices = Json::array();
    for (const auto value : identity.original_instrument_indices) {
        indices.push_back(std::to_string(value));
    }
    return Json{{"date_encoding", kPanelSessionEncoding},
                {"date_semantics", kPanelSessionSemantics},
                {"instrument_namespace", identity.instrument_namespace},
                {"session_keys", std::move(dates)},
                {"instrument_ids", identity.instrument_ids},
                {"original_instrument_indices", std::move(indices)}};
}

[[nodiscard]] Json parents_json(const PanelIdentity &identity) {
    auto parents = identity.parents;
    std::sort(parents.begin(), parents.end(), [](const PanelParent &a, const PanelParent &b) {
        return a.role < b.role;
    });
    Json result = Json::array();
    for (const auto &parent : parents) {
        result.push_back(Json{{"role", parent.role}, {"sha256", parent.sha256}});
    }
    return result;
}

// Versioned deterministic nlohmann object serialization, not RFC 8785/JCS.
// No floats occur in the identity schema; recipe text is hashed byte-for-byte.
[[nodiscard]] Result<Json> manifest_body(const PanelIdentity &identity,
                                        const std::vector<std::string> &fields,
                                        const std::string &payload_hash, atx::u64 payload_size,
                                        atx::u64 payload_digest) {
    const Json axes = axes_json(identity);
    const Json parents = parents_json(identity);
    ATX_TRY(auto axes_hash, atx::core::sha256_hex(axes.dump()));
    ATX_TRY(auto recipe_hash, atx::core::sha256_hex(identity.recipe));
    ATX_TRY(auto parents_hash, atx::core::sha256_hex(parents.dump()));
    return Ok(Json{
        {"schema", "atx.panel-artifact"}, {"schema_version", 1U},
        {"axes", axes}, {"recipe", identity.recipe}, {"parents", parents},
        {"shape", {{"dates", std::to_string(identity.session_keys.size())},
                   {"instruments", std::to_string(identity.instrument_ids.size())},
                   {"fields", fields}}},
        {"payload", {{"format", "APNLv1"}, {"size_bytes", std::to_string(payload_size)},
                     {"sha256", payload_hash}, {"fnv1a64", to_hex16(payload_digest)}}},
        {"hashes", {{"axes_sha256", axes_hash}, {"recipe_sha256", recipe_hash},
                    {"parents_sha256", parents_hash}}}});
}

[[nodiscard]] Result<std::string> identity_hash(const Json &body) {
    return atx::core::sha256_hex(std::string(kIdentityDomain) + body.dump());
}

[[nodiscard]] Result<std::string> read_manifest_text(const fs::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return Err(ErrorCode::IoError, "panel artifact: manifest missing/unreadable");
    const auto length = input.tellg();
    if (length <= 0 || length > static_cast<std::streamoff>(kMaxManifestBytes)) {
        return Err(ErrorCode::ParseError, "panel artifact: invalid manifest byte size");
    }
    std::string result(static_cast<atx::usize>(length), '\0');
    input.seekg(0);
    input.read(result.data(), static_cast<std::streamsize>(result.size()));
    if (!input) return Err(ErrorCode::IoError, "panel artifact: manifest read failed");
    return Ok(std::move(result));
}

[[nodiscard]] Json parse_manifest(const std::string &text) {
    std::vector<std::set<std::string>> object_keys;
    return Json::parse(text, [&object_keys](int depth, Json::parse_event_t event, Json &value) {
        if (depth > kMaxJsonDepth) throw std::invalid_argument("manifest nesting limit exceeded");
        if (event == Json::parse_event_t::object_start) object_keys.emplace_back();
        if (event == Json::parse_event_t::key) {
            if (object_keys.empty() ||
                !object_keys.back().insert(value.get<std::string>()).second) {
                throw std::invalid_argument("duplicate manifest object key");
            }
        }
        if (event == Json::parse_event_t::object_end) object_keys.pop_back();
        return true;
    });
}

[[nodiscard]] Result<PanelIdentity> read_identity(const Json &document) {
    PanelIdentity result;
    const auto &axes = document.at("axes");
    result.instrument_namespace = axes.at("instrument_namespace").get<std::string>();
    result.instrument_ids = axes.at("instrument_ids").get<std::vector<std::string>>();
    for (const auto &item : axes.at("session_keys")) {
        ATX_TRY(auto date, decimal<atx::i64>(item.get<std::string>()));
        result.session_keys.push_back(date);
    }
    for (const auto &item : axes.at("original_instrument_indices")) {
        ATX_TRY(auto index, decimal<atx::usize>(item.get<std::string>()));
        result.original_instrument_indices.push_back(index);
    }
    result.recipe = document.at("recipe").get<std::string>();
    for (const auto &item : document.at("parents")) {
        result.parents.push_back({item.at("role").get<std::string>(),
                                  item.at("sha256").get<std::string>()});
    }
    ATX_TRY_VOID(validate_identity(result));
    return Ok(std::move(result));
}

[[nodiscard]] Result<atx::u64> expected_payload_size(const PanelIdentity &identity,
                                                   const std::vector<std::string> &fields) {
    const auto dates = static_cast<atx::u64>(identity.session_keys.size());
    const auto instruments = static_cast<atx::u64>(identity.instrument_ids.size());
    const auto field_count = static_cast<atx::u64>(fields.size());
    const auto limit = std::min({
        static_cast<atx::u64>(std::numeric_limits<std::streamsize>::max()),
        static_cast<atx::u64>(std::numeric_limits<std::streamoff>::max()),
        static_cast<atx::u64>(std::numeric_limits<atx::usize>::max())});
    if (dates > limit / instruments) {
        return Err(ErrorCode::OutOfRange, "panel artifact: cell count overflow");
    }
    const atx::u64 cells = dates * instruments;
    const atx::u64 stride = 8U * field_count + 1U;
    atx::u64 size = 40U; // APNL header (32) plus legacy FNV trailer (8).
    for (const auto &field : fields) size += 4U + static_cast<atx::u64>(field.size());
    if (cells > (limit - size) / stride) {
        return Err(ErrorCode::OutOfRange, "panel artifact: payload byte count overflow");
    }
    return Ok(size + cells * stride);
}

template <atx::usize Bytes>
[[nodiscard]] Result<atx::u64> read_le(std::ifstream &input) {
    std::array<unsigned char, Bytes> bytes{};
    // SAFETY: char accesses object representation; fixed-size storage is writable.
    input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(Bytes));
    if (!input) return Err(ErrorCode::ParseError, "panel artifact: truncated APNL header");
    atx::u64 value{};
    for (atx::usize i = 0; i < Bytes; ++i) value |= atx::u64{bytes[i]} << (8U * i);
    return Ok(value);
}

// Check structural bounds before the legacy reader can reserve based on file data.
[[nodiscard]] Status preflight_payload(const fs::path &path, const PanelIdentity &identity,
                                       const std::vector<std::string> &fields,
                                       atx::u64 declared_size) {
    ATX_TRY(auto expected_size, expected_payload_size(identity, fields));
    if (declared_size != expected_size || fs::file_size(path) != expected_size) {
        return Err(ErrorCode::ParseError, "panel artifact: payload byte size mismatch");
    }
    std::ifstream input(path, std::ios::binary);
    ATX_TRY(auto magic, read_le<4>(input));
    ATX_TRY(auto version, read_le<4>(input));
    ATX_TRY(auto dates, read_le<8>(input));
    ATX_TRY(auto instruments, read_le<8>(input));
    ATX_TRY(auto field_count, read_le<8>(input));
    if (magic != 0x4c4e5041U || version != 1 || dates != identity.session_keys.size() ||
        instruments != identity.instrument_ids.size() || field_count != fields.size()) {
        return Err(ErrorCode::ParseError, "panel artifact: APNL shape/header mismatch");
    }
    for (const auto &field : fields) {
        ATX_TRY(auto length, read_le<4>(input));
        if (length != field.size()) {
            return Err(ErrorCode::ParseError, "panel artifact: APNL field length mismatch");
        }
        std::string actual(field.size(), '\0');
        input.read(actual.data(), static_cast<std::streamsize>(actual.size()));
        if (!input || actual != field) {
            return Err(ErrorCode::ParseError, "panel artifact: APNL field name mismatch");
        }
    }
    return Ok();
}

// Read exactly once. A path changing before/during the read cannot substitute
// different decoded content after its SHA has been verified: all later operations
// consume this owned snapshot, never reopen the path.
[[nodiscard]] Result<std::vector<atx::u8>> read_payload_snapshot(const fs::path &path,
                                                               atx::u64 expected_size,
                                                               atx::u64 max_payload_bytes) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return Err(ErrorCode::ParseError, "panel artifact: payload byte size mismatch");
    }
    const auto opened_size = static_cast<std::streamoff>(input.tellg());
    if (opened_size < 0) {
        return Err(ErrorCode::ParseError, "panel artifact: payload byte size mismatch");
    }
    if (static_cast<atx::u64>(opened_size) > max_payload_bytes) {
        return Err(ErrorCode::OutOfRange, "panel artifact: opened payload exceeds byte budget");
    }
    if (static_cast<atx::u64>(opened_size) != expected_size) {
        return Err(ErrorCode::ParseError, "panel artifact: payload byte size mismatch");
    }
    std::vector<atx::u8> bytes(static_cast<atx::usize>(expected_size));
    input.seekg(0);
    // SAFETY: char accesses the writable representation of this bounded byte buffer.
    input.read(reinterpret_cast<char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof()) {
        return Err(ErrorCode::ParseError, "panel artifact: cannot read complete payload snapshot");
    }
    return Ok(std::move(bytes));
}

template <atx::usize Bytes>
[[nodiscard]] Result<atx::u64> read_le(std::span<const atx::u8> bytes, atx::usize &position) {
    if (position > bytes.size() || Bytes > bytes.size() - position) {
        return Err(ErrorCode::ParseError, "panel artifact: truncated APNL snapshot");
    }
    atx::u64 value{};
    for (atx::usize i = 0; i < Bytes; ++i) {
        value |= atx::u64{bytes[position + i]} << (8U * i);
    }
    position += Bytes;
    return Ok(value);
}

// Validate the captured layout before the numeric decoder allocates its columns.
[[nodiscard]] Status preflight_payload(std::span<const atx::u8> bytes,
                                       const PanelIdentity &identity,
                                       const std::vector<std::string> &fields) {
    atx::usize position = 0;
    ATX_TRY(auto magic, read_le<4>(bytes, position));
    ATX_TRY(auto version, read_le<4>(bytes, position));
    ATX_TRY(auto dates, read_le<8>(bytes, position));
    ATX_TRY(auto instruments, read_le<8>(bytes, position));
    ATX_TRY(auto field_count, read_le<8>(bytes, position));
    if (magic != 0x4c4e5041U || version != 1 || dates != identity.session_keys.size() ||
        instruments != identity.instrument_ids.size() || field_count != fields.size()) {
        return Err(ErrorCode::ParseError, "panel artifact: APNL shape/header mismatch");
    }
    for (const auto &field : fields) {
        ATX_TRY(auto length, read_le<4>(bytes, position));
        if (length != field.size() || length > bytes.size() - position) {
            return Err(ErrorCode::ParseError, "panel artifact: APNL field length mismatch");
        }
        // SAFETY: char reads the bounded field-name bytes without changing them.
        const std::string_view actual(reinterpret_cast<const char *>(bytes.data() + position),
                                      field.size());
        if (actual != field) {
            return Err(ErrorCode::ParseError, "panel artifact: APNL field name mismatch");
        }
        position += field.size();
    }
    return Ok();
}

[[nodiscard]] Status write_text(const fs::path &path, const std::string &text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    if (!output) return Err(ErrorCode::IoError, "panel artifact: manifest write failed");
    output.close();
    if (!output) return Err(ErrorCode::IoError, "panel artifact: manifest close failed");
    return Ok();
}

// Hard-link creation is atomic and never replaces an existing destination. Both
// names are in one directory; unsupported filesystems fail without publishing.
void publish_fresh(const fs::path &temporary, const fs::path &final) {
    fs::create_hard_link(temporary, final);
    if (!fs::remove(temporary)) throw std::runtime_error("cannot remove published temporary");
}

} // namespace

Result<PanelArtifactReceipt> write_panel_artifact(const atx::engine::alpha::Panel &panel,
                                                 const std::string &path,
                                                 const PanelIdentity &identity) {
    try {
        ATX_TRY_VOID(validate_identity(identity));
        if (path.empty() || panel.dates() != identity.session_keys.size() ||
            panel.instruments() != identity.instrument_ids.size()) {
            return Err(ErrorCode::InvalidArgument, "panel artifact: path/axis shape mismatch");
        }
        std::vector<std::string> fields;
        for (atx::usize i = 0; i < panel.num_fields(); ++i) fields.push_back(panel.field_name(i));
        ATX_TRY_VOID(validate_fields(fields));
        ATX_TRY(auto expected_size, expected_payload_size(identity, fields));
        const fs::path final(path);
        const fs::path manifest(path + ".manifest.json");
        const fs::path temporary(path + ".partial");
        const fs::path manifest_temporary(path + ".manifest.json.partial");
        const fs::path lock(path + ".publishing");
        if (!final.parent_path().empty()) fs::create_directories(final.parent_path());
        if (!fs::create_directory(lock)) {
            return Err(ErrorCode::AlreadyExists, "panel artifact: publication already reserved");
        }
        for (const auto &candidate : {final, manifest, temporary, manifest_temporary}) {
            if (fs::symlink_status(candidate).type() != fs::file_type::not_found) {
                return Err(ErrorCode::AlreadyExists, "panel artifact: output already exists");
            }
        }
        ATX_TRY(auto legacy_digest, write_panel(panel, temporary.string()));
        ATX_TRY_VOID(preflight_payload(temporary, identity, fields, expected_size));
        ATX_TRY(auto payload_hash, atx::core::sha256_file(temporary.string()));
        ATX_TRY(auto body, manifest_body(identity, fields, payload_hash, expected_size,
                                          legacy_digest));
        ATX_TRY(auto artifact_id, identity_hash(body));
        body["artifact_id"] = artifact_id;
        const std::string text = body.dump(2) + "\n";
        if (text.size() > kMaxManifestBytes) {
            return Err(ErrorCode::OutOfRange, "panel artifact: manifest exceeds byte limit");
        }
        ATX_TRY_VOID(write_text(manifest_temporary, text));
        publish_fresh(temporary, final);
        publish_fresh(manifest_temporary, manifest); // Commit marker, always last.
        if (!fs::remove(lock)) {
            return Err(ErrorCode::IoError, "panel artifact: lock cleanup failed");
        }
        return Ok(PanelArtifactReceipt{
            std::move(artifact_id), std::move(payload_hash), legacy_digest});
    } catch (const std::exception &error) {
        return Err(ErrorCode::IoError, std::string("panel artifact write: ") + error.what());
    }
}

Result<PanelArtifact> read_panel_artifact(const std::string &path) {
    return read_panel_artifact(path, std::numeric_limits<atx::u64>::max());
}

Result<PanelArtifact> read_panel_artifact(const std::string &path, atx::u64 max_payload_bytes) {
    try {
        if (path.empty()) return Err(ErrorCode::InvalidArgument, "panel artifact: empty path");
        ATX_TRY(auto text, read_manifest_text(path + ".manifest.json"));
        Json document = parse_manifest(text);
        if (!document.is_object() || document.at("schema") != "atx.panel-artifact" ||
            !document.at("schema_version").is_number_unsigned() ||
            document.at("schema_version").get<atx::u64>() != 1U) {
            return Err(ErrorCode::ParseError, "panel artifact: unsupported manifest schema");
        }
        ATX_TRY(auto identity, read_identity(document));
        auto fields = document.at("shape").at("fields").get<std::vector<std::string>>();
        ATX_TRY_VOID(validate_fields(fields));
        const auto &payload = document.at("payload");
        const std::string payload_hash = payload.at("sha256").get<std::string>();
        const std::string artifact_id = document.at("artifact_id").get<std::string>();
        const std::string fnv = payload.at("fnv1a64").get<std::string>();
        if (!hash_valid(payload_hash) || !hash_valid(artifact_id) || !hash_valid(fnv, 16)) {
            return Err(ErrorCode::ParseError, "panel artifact: invalid digest text");
        }
        atx::u64 legacy_digest{};
        const auto parsed = std::from_chars(fnv.data(), fnv.data() + fnv.size(), legacy_digest, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != fnv.data() + fnv.size()) {
            return Err(ErrorCode::ParseError, "panel artifact: invalid legacy digest");
        }
        ATX_TRY(auto payload_size, decimal<atx::u64>(payload.at("size_bytes").get<std::string>()));
        if (payload_size > max_payload_bytes) {
            return Err(ErrorCode::OutOfRange,
                       "panel artifact: declared payload exceeds byte budget");
        }
        ATX_TRY(auto expected, manifest_body(identity, fields, payload_hash, payload_size,
                                             legacy_digest));
        ATX_TRY(auto expected_id, identity_hash(expected));
        expected["artifact_id"] = expected_id;
        // Structural equality also rejects unknown keys, wrong array/object types,
        // altered shape/semantics, and stale component hashes without ad hoc parsing.
        if (document != expected || artifact_id != expected_id) {
            return Err(ErrorCode::ParseError, "panel artifact: manifest identity mismatch");
        }
        ATX_TRY(auto expected_size, expected_payload_size(identity, fields));
        if (payload_size != expected_size) {
            return Err(ErrorCode::ParseError, "panel artifact: payload byte size mismatch");
        }
        ATX_TRY(auto snapshot, read_payload_snapshot(path, expected_size, max_payload_bytes));
        const std::span<const atx::u8> bytes(snapshot);
        ATX_TRY_VOID(preflight_payload(bytes, identity, fields));
        ATX_TRY(auto actual_hash, atx::core::sha256_hex(std::as_bytes(bytes)));
        if (actual_hash != payload_hash) {
            return Err(ErrorCode::ParseError, "panel artifact: payload SHA-256 mismatch");
        }
        // The manifest binding and the decoder's FNV consistency check cover
        // exactly the same captured trailer/payload as the verified SHA above.
        atx::usize trailer_position = bytes.size() - 8U;
        ATX_TRY(auto actual_digest, read_le<8>(bytes, trailer_position));
        if (actual_digest != legacy_digest) {
            return Err(ErrorCode::ParseError, "panel artifact: legacy digest binding mismatch");
        }
        ATX_TRY(auto panel, read_panel_bytes(bytes));
        return Ok(PanelArtifact{std::move(panel), std::move(identity), artifact_id, payload_hash});
    } catch (const std::exception &error) {
        return Err(ErrorCode::ParseError, std::string("panel artifact read: ") + error.what());
    }
}

Status require_same_panel_axes(const PanelIdentity &left, const PanelIdentity &right) {
    try {
        ATX_TRY_VOID(validate_identity(left));
        ATX_TRY_VOID(validate_identity(right));
        if (left.instrument_namespace != right.instrument_namespace ||
            left.session_keys != right.session_keys ||
            left.instrument_ids != right.instrument_ids ||
            left.original_instrument_indices != right.original_instrument_indices) {
            return Err(ErrorCode::InvalidArgument, "panel artifact: ordered axes differ");
        }
        return Ok();
    } catch (const std::exception &error) {
        return Err(ErrorCode::InvalidArgument, std::string("panel artifact axes: ") + error.what());
    }
}

Status require_panel_parent(const PanelIdentity &child, std::string_view role,
                            std::string_view parent_artifact_id) {
    try {
        ATX_TRY_VOID(validate_identity(child));
        if (!hash_valid(parent_artifact_id)) {
            return Err(ErrorCode::InvalidArgument,
                       "panel artifact: invalid expected parent digest");
        }
        for (const auto &parent : child.parents) {
            if (parent.role == role && parent.sha256 == parent_artifact_id) return Ok();
        }
        return Err(ErrorCode::InvalidArgument, "panel artifact: required parent binding missing");
    } catch (const std::exception &error) {
        return Err(ErrorCode::InvalidArgument,
                   std::string("panel artifact parent: ") + error.what());
    }
}

Result<PanelStoreWindow> read_panel_store_window(const std::string& directory,
    atx::usize begin, atx::usize end, atx::u64 budget, std::string_view expected) {
    using namespace atx::engine;
    ATX_TRY(auto store, data::PanelStore::open(directory, expected));
    const auto& c = store.config();
    if (begin >= end || end > c.session_keys.size())
        return Err(ErrorCode::InvalidArgument, "panel store: invalid materialization window");
    const auto n = c.instrument_ids.size(), d = end - begin;
    const atx::u64 cells = atx::u64{n} * d;
    const atx::u64 bytes_per_cell = c.fields.size() * sizeof(atx::f64) + 2;
    const atx::u64 metadata = 32ULL * 1024 * 1024 + atx::u64{n} * 128;
    if (budget < metadata || cells > (budget - metadata) / bytes_per_cell ||
        cells > (std::numeric_limits<atx::usize>::max)())
        return Err(ErrorCode::InvalidArgument, "panel store: f64 window materialization budget exceeded");
    const auto count = static_cast<atx::usize>(cells);
    std::vector<std::string> names;
    std::vector<std::vector<atx::f64>> columns(c.fields.size());
    for (atx::usize f = 0; f < c.fields.size(); ++f) { names.push_back(c.fields[f].name); columns[f].resize(count); }
    std::vector<atx::u8> present(count), tradable(count);
    for (atx::usize index = begin / c.chunk_dates; index <= (end - 1) / c.chunk_dates; ++index) {
        ATX_TRY(auto chunk, store.open_chunk(index));
        const auto first = std::max(begin, chunk.begin_date());
        const auto last = std::min(end, chunk.begin_date() + chunk.dates());
        for (atx::usize date = first; date < last; ++date) {
            const auto local = date - chunk.begin_date(), offset = (date - begin) * n;
            for (atx::usize f = 0; f < c.fields.size(); ++f) {
                auto row = std::span<atx::f64>(columns[f]).subspan(offset, n);
                if (c.fields[f].name == "close") ATX_TRY_VOID(chunk.read_exact_close_row(local, row));
                else ATX_TRY_VOID(chunk.read_field_row(f, local, row));
            }
            ATX_TRY(auto p, chunk.present(local)); ATX_TRY(auto m, chunk.tradable(local));
            std::copy(p.begin(), p.end(), present.begin() + static_cast<std::ptrdiff_t>(offset));
            std::copy(m.begin(), m.end(), tradable.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    }
    ATX_TRY(auto panel, alpha::Panel::create(d, n, std::move(names), std::move(columns), std::move(present)));
    PanelIdentity identity; identity.instrument_namespace = c.instrument_namespace;
    identity.session_keys.assign(c.session_keys.begin() + static_cast<std::ptrdiff_t>(begin),
                                 c.session_keys.begin() + static_cast<std::ptrdiff_t>(end));
    for (auto id : c.instrument_ids) identity.instrument_ids.push_back(std::to_string(id));
    for (auto index : c.original_indices) {
        if (index > (std::numeric_limits<atx::usize>::max)())
            return Err(ErrorCode::InvalidArgument, "panel store: source index does not fit consumer");
        identity.original_instrument_indices.push_back(static_cast<atx::usize>(index));
    }
    const std::string manifest(store.manifest_sha256());
    identity.recipe = Json{{"rule", "panel-store-window-v2"}, {"begin", begin}, {"end", end},
        {"source_recipe", c.recipe}, {"close_read", "original-f64-adjusted"},
        {"mask", "source-presence;separate-dated-tradable"}}.dump();
    identity.parents = {{"panel_store", manifest}, {"membership", c.membership_sha256}};
    ATX_TRY(auto id, atx::core::sha256_hex("atx-panel-store-window-v2\n" + manifest + "\n" + identity.recipe));
    return Ok(PanelStoreWindow{PanelArtifact{std::move(panel), std::move(identity), std::move(id), manifest},
                               std::move(tradable), c.fields});
}

} // namespace atx::impl
