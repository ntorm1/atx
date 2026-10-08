#include <atx/server/access.hpp>

#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace atx::server {
namespace {
using Json = nlohmann::json;
using Digest = std::array<unsigned char, 32>;
bool identifier_char(char ch) noexcept {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
}
void keys(const Json& object, std::initializer_list<std::string_view> allowed) {
    if (!object.is_object()) throw std::invalid_argument("team configuration requires objects");
    for (const auto& item : object.items())
        if (std::find(allowed.begin(), allowed.end(), item.key()) == allowed.end())
            throw std::invalid_argument("unknown team configuration field");
}
std::string text(const Json& object, const char* key, std::size_t maximum) {
    const auto result = object.at(key).get<std::string>();
    if (result.empty() || result.size() > maximum ||
        std::all_of(result.begin(), result.end(), [](char ch) { return ch == ' '; }) ||
        std::any_of(result.begin(), result.end(), [](unsigned char ch) {
            return ch < 32 || ch == 127;
        })) throw std::invalid_argument("invalid team configuration text");
    return result;
}
TeamRole parse_role(std::string_view role) {
    if (role == "viewer") return TeamRole::viewer;
    if (role == "researcher") return TeamRole::researcher;
    if (role == "reviewer") return TeamRole::reviewer;
    if (role == "admin") return TeamRole::admin;
    throw std::invalid_argument("unknown team role");
}
Digest parse_digest(std::string_view value) {
    if (value.size() != 64)
        throw std::invalid_argument("credential digest must have 64 hex digits");
    const auto hex = [](char ch) -> unsigned char {
        if (ch >= '0' && ch <= '9') return static_cast<unsigned char>(ch - '0');
        if (ch >= 'a' && ch <= 'f') return static_cast<unsigned char>(ch - 'a' + 10);
        throw std::invalid_argument("credential digest must use lowercase hexadecimal");
    };
    Digest result{};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<unsigned char>((hex(value[2 * i]) << 4) | hex(value[2 * i + 1]));
    return result;
}
Digest digest(std::string_view value) {
    Digest result{};
    unsigned int bytes{};
    if (EVP_Digest(value.data(), value.size(), result.data(), &bytes, EVP_sha256(), nullptr) != 1 ||
        bytes != result.size()) throw std::runtime_error("credential digest unavailable");
    return result;
}
std::int64_t expiry(const Json& object) {
    if (!object.contains("expires_at_ms")) return 0;
    const auto& value = object.at("expires_at_ms");
    if (!value.is_number_integer() || (value.is_number_unsigned() &&
        value.get<std::uint64_t>() >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
        throw std::invalid_argument("credential expiry must be a nonnegative int64 timestamp");
    const auto result = value.get<std::int64_t>();
    if (result < 0) throw std::invalid_argument("credential expiry must be nonnegative");
    return result;
}
Json document(std::string_view source) {
    if (source.empty() || source.size() > 65536)
        throw std::invalid_argument("team configuration exceeds 64 KiB or is empty");
    std::vector<std::set<std::string>> objects;
    return Json::parse(source, [&](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 8) throw std::invalid_argument("team configuration nesting is too deep");
        if (event == Json::parse_event_t::object_start) objects.emplace_back();
        if (event == Json::parse_event_t::key &&
            !objects.back().insert(value.get<std::string>()).second)
            throw std::invalid_argument("team configuration has duplicate keys");
        if (event == Json::parse_event_t::object_end) objects.pop_back();
        return true;
    });
}
} // namespace

struct AccessPolicy::Impl {
    struct Credential { Principal principal; Digest token_digest; };
    std::string name;
    std::vector<Credential> credentials;
    Impl(std::string_view source, std::int64_t now) {
        if (now < 0) throw std::invalid_argument("invalid policy clock");
        const auto json = document(source);
        keys(json, {"schema_version", "team_name", "principals"});
        if (!json.at("schema_version").is_number_integer() || json.at("schema_version") != 1)
            throw std::invalid_argument("unsupported team configuration version");
        name = text(json, "team_name", 128);
        const auto& members = json.at("principals");
        if (!members.is_array() || members.empty() || members.size() > 128)
            throw std::invalid_argument("team configuration requires 1..128 principals");
        std::set<std::string> ids;
        std::set<Digest> digests;
        bool active_admin = false;
        credentials.reserve(members.size());
        for (const auto& member : members) {
            keys(member, {"id", "name", "role", "token_sha256", "expires_at_ms"});
            auto id = text(member, "id", 64);
            if (!std::all_of(id.begin(), id.end(), identifier_char) ||
                id == "local" || id == "gateway" || id == "system" || id == "migration_unknown" ||
                !ids.insert(id).second)
                throw std::invalid_argument(
                    "principal IDs must be distinct unreserved identifiers");
            auto principal = Principal{std::move(id), text(member, "name", 128),
                parse_role(text(member, "role", 16)), expiry(member)};
            const auto hash = parse_digest(text(member, "token_sha256", 64));
            if (!digests.insert(hash).second)
                throw std::invalid_argument("principals must have distinct credential digests");
            active_admin = active_admin || (principal.role == TeamRole::admin &&
                (principal.expires_at_ms == 0 || principal.expires_at_ms > now));
            credentials.push_back({std::move(principal), hash});
        }
        if (!active_admin) throw std::invalid_argument("team requires an unexpired administrator");
    }
};
AccessPolicy::AccessPolicy(std::string_view source, std::int64_t now) {
    try {
        impl_ = std::make_unique<Impl>(source, now);
    } catch (const Json::exception&) {
        // Parser diagnostics may echo configuration values. Keep startup errors credential-free.
        throw std::invalid_argument("malformed team configuration");
    }
}
AccessPolicy::~AccessPolicy() = default;
bool AccessPolicy::contains_credential(std::string_view authorization) const {
    if (!authorization.starts_with("Bearer ")) return false;
    const auto token = authorization.substr(7);
    if (token.size() < 32 || token.size() > 256) return false;
    const auto presented = digest(token);
    bool matched = false;
    for (const auto& credential : impl_->credentials) {
        // Expired user secrets must never become privileged service credentials at startup.
        const auto equal = CRYPTO_memcmp(presented.data(), credential.token_digest.data(),
                                        presented.size()) == 0;
        matched = matched | equal;
    }
    return matched;
}
std::optional<Principal> AccessPolicy::authenticate(std::string_view authorization,
                                                  std::int64_t now) const {
    if (now < 0 || !authorization.starts_with("Bearer ")) return std::nullopt;
    const auto token = authorization.substr(7);
    if (token.size() < 32 || token.size() > 256 ||
        !std::all_of(token.begin(), token.end(), identifier_char)) return std::nullopt;
    const auto presented = digest(token);
    const Principal* matched = nullptr;
    for (const auto& credential : impl_->credentials) {
        // Scan the complete bounded set, comparing fixed-size digests in constant time.
        const auto equal = CRYPTO_memcmp(presented.data(), credential.token_digest.data(),
                                        presented.size()) == 0;
        if (equal && (credential.principal.expires_at_ms == 0 ||
            credential.principal.expires_at_ms > now)) matched = &credential.principal;
    }
    return matched == nullptr ? std::nullopt : std::optional<Principal>(*matched);
}
std::vector<Principal> AccessPolicy::principals() const {
    std::vector<Principal> result;
    result.reserve(impl_->credentials.size());
    for (const auto& credential : impl_->credentials) result.push_back(credential.principal);
    return result;
}
std::string_view AccessPolicy::team_name() const noexcept { return impl_->name; }
std::string_view role_name(TeamRole role) noexcept {
    switch (role) {
    case TeamRole::viewer: return "viewer";
    case TeamRole::researcher: return "researcher";
    case TeamRole::reviewer: return "reviewer";
    case TeamRole::admin: return "admin";
    }
    return "unknown";
}
bool permits(TeamRole role, Permission permission) noexcept {
    switch (permission) {
    case Permission::read:
        return role == TeamRole::viewer || role == TeamRole::researcher ||
            role == TeamRole::reviewer || role == TeamRole::admin;
    case Permission::research:
        return role == TeamRole::researcher || role == TeamRole::reviewer ||
            role == TeamRole::admin;
    case Permission::review: return role == TeamRole::reviewer || role == TeamRole::admin;
    case Permission::administer: return role == TeamRole::admin;
    }
    return false;
}
} // namespace atx::server
