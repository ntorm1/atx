#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atx::server {
enum class TeamRole { viewer, researcher, reviewer, admin };
enum class Permission { read, research, review, administer };
struct Principal {
    std::string id;
    std::string name;
    TeamRole role{TeamRole::viewer};
    std::int64_t expires_at_ms{};
};
[[nodiscard]] std::string_view role_name(TeamRole role) noexcept;
[[nodiscard]] bool permits(TeamRole role, Permission permission) noexcept;

// Immutable startup policy for a single shared team. Tokens never leave this boundary.
// Configuration stores only SHA256 digests of operator-issued random bearer tokens.
class AccessPolicy final {
public:
    explicit AccessPolicy(std::string_view document, std::int64_t startup_time_ms);
    ~AccessPolicy();
    AccessPolicy(const AccessPolicy&) = delete;
    AccessPolicy& operator=(const AccessPolicy&) = delete;
    AccessPolicy(AccessPolicy&&) = delete;
    AccessPolicy& operator=(AccessPolicy&&) = delete;
    [[nodiscard]] std::optional<Principal> authenticate(std::string_view authorization,
                                                       std::int64_t now_ms) const;
    // Startup credential separation includes expired principals, whose secrets may remain known.
    [[nodiscard]] bool contains_credential(std::string_view authorization) const;
    [[nodiscard]] std::vector<Principal> principals() const;
    [[nodiscard]] std::string_view team_name() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace atx::server
