#include <atx/server/access.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <array>
#include <stdexcept>
#include <string>

namespace atx::server {
namespace {
using Json = nlohmann::json;
std::string digest(std::string_view token) {
    std::array<unsigned char, 32> bytes{};
    unsigned int count{};
    if (EVP_Digest(token.data(), token.size(), bytes.data(), &count, EVP_sha256(), nullptr) != 1 ||
        count != bytes.size()) throw std::runtime_error("test digest failure");
    std::string result;
    for (const auto byte : bytes) {
        result.push_back("0123456789abcdef"[byte >> 4]);
        result.push_back("0123456789abcdef"[byte & 15]);
    }
    return result;
}
Json configuration() {
    auto members = Json::array();
    for (const auto* role : {"viewer", "researcher", "reviewer", "admin"}) {
        members.push_back({{"id", role}, {"name", std::string("Team ") + role}, {"role", role},
            {"token_sha256", digest(std::string(32, role[0]))}, {"expires_at_ms", 0}});
    }
    // Researcher and reviewer must have distinct tokens despite their common first letter.
    members[2]["token_sha256"] = digest(std::string(32, 'x'));
    return {{"schema_version", 1}, {"team_name", "Equity research"}, {"principals", members}};
}

TEST(TeamAccess, ExactCredentialResolvesPrincipalAndMetadataNeverExposesDigests) {
    const AccessPolicy policy(configuration().dump(), 1000);
    const auto principal = policy.authenticate("Bearer " + std::string(32, 'v'), 1000);
    ASSERT_TRUE(principal);
    EXPECT_EQ(principal->id, "viewer");
    EXPECT_EQ(principal->role, TeamRole::viewer);
    EXPECT_EQ(policy.principals().size(), 4U);
    EXPECT_EQ(policy.team_name(), "Equity research");
    EXPECT_FALSE(policy.authenticate("Bearer " + std::string(32, 'z'), 1000));
    EXPECT_FALSE(policy.authenticate("bearer " + std::string(32, 'v'), 1000));
    EXPECT_FALSE(policy.authenticate("Bearer " + std::string(31, 'v'), 1000));
    EXPECT_FALSE(policy.authenticate("Bearer " + std::string(257, 'v'), 1000));
    EXPECT_FALSE(policy.authenticate("Bearer " + std::string(32, 'v') + " ", 1000));
}

TEST(TeamAccess, RolesHaveExplicitReadResearchReviewAndAdministrationPermissions) {
    for (const auto role : {TeamRole::viewer, TeamRole::researcher, TeamRole::reviewer,
                            TeamRole::admin}) EXPECT_TRUE(permits(role, Permission::read));
    EXPECT_FALSE(permits(TeamRole::viewer, Permission::research));
    EXPECT_TRUE(permits(TeamRole::researcher, Permission::research));
    EXPECT_FALSE(permits(TeamRole::researcher, Permission::review));
    EXPECT_TRUE(permits(TeamRole::reviewer, Permission::review));
    EXPECT_FALSE(permits(TeamRole::reviewer, Permission::administer));
    EXPECT_TRUE(permits(TeamRole::admin, Permission::administer));
    EXPECT_EQ(role_name(TeamRole::reviewer), "reviewer");
}

TEST(TeamAccess, ExpiryIsEnforcedOnEveryAuthenticationAndAnActiveAdminIsRequired) {
    auto document = configuration();
    document["principals"][0]["expires_at_ms"] = 2000;
    const AccessPolicy policy(document.dump(), 1000);
    EXPECT_TRUE(policy.authenticate("Bearer " + std::string(32, 'v'), 1999));
    EXPECT_FALSE(policy.authenticate("Bearer " + std::string(32, 'v'), 2000));
    document["principals"][3]["expires_at_ms"] = 1000;
    EXPECT_THROW((void)AccessPolicy(document.dump(), 1000), std::invalid_argument);
    document["principals"][3]["expires_at_ms"] = 1.5;
    EXPECT_THROW((void)AccessPolicy(document.dump(), 1000), std::invalid_argument);
}
TEST(TeamAccess, ServiceCredentialCannotReuseAnExpiredPrincipalSecret) {
    auto document = configuration();
    document["principals"][0]["expires_at_ms"] = 1000;
    const AccessPolicy policy(document.dump(), 2000);
    const auto expired = "Bearer " + std::string(32, 'v');
    EXPECT_FALSE(policy.authenticate(expired, 2000));
    EXPECT_TRUE(policy.contains_credential(expired));
    EXPECT_TRUE(policy.contains_credential("Bearer " + std::string(32, 'a')));
    EXPECT_FALSE(policy.contains_credential("Bearer " + std::string(32, 'z')));
    EXPECT_FALSE(policy.contains_credential("bearer " + std::string(32, 'v')));
    EXPECT_FALSE(policy.contains_credential("Bearer " + std::string(257, 'v')));
}

TEST(TeamAccess, AmbiguousIdentitiesSecretsAndUnknownConfigurationAreRejected) {
    auto original = configuration();
    auto duplicate_id = original;
    duplicate_id["principals"][1]["id"] = "viewer";
    EXPECT_THROW((void)AccessPolicy(duplicate_id.dump(), 1000), std::invalid_argument);
    auto duplicate_token = original;
    duplicate_token["principals"][1]["token_sha256"] =
        duplicate_token["principals"][0]["token_sha256"];
    EXPECT_THROW((void)AccessPolicy(duplicate_token.dump(), 1000), std::invalid_argument);
    auto plaintext = original;
    plaintext["principals"][0]["token"] = std::string(32, 'v');
    EXPECT_THROW((void)AccessPolicy(plaintext.dump(), 1000), std::invalid_argument);
    auto malformed = original;
    malformed["principals"][0]["token_sha256"] = std::string(64, 'z');
    EXPECT_THROW((void)AccessPolicy(malformed.dump(), 1000), std::invalid_argument);
    malformed = original;
    malformed["principals"][0]["id"] = "migration_unknown";
    EXPECT_THROW((void)AccessPolicy(malformed.dump(), 1000), std::invalid_argument);
    malformed = original;
    malformed["principals"][0]["role"] = "owner";
    EXPECT_THROW((void)AccessPolicy(malformed.dump(), 1000), std::invalid_argument);
    EXPECT_THROW((void)AccessPolicy("{\"schema_version\":1,\"schema_version\":1}", 1000),
                 std::invalid_argument);
    EXPECT_THROW((void)AccessPolicy(std::string(65537, ' '), 1000), std::invalid_argument);
    original["principals"] = Json::array();
    EXPECT_THROW((void)AccessPolicy(original.dump(), 1000), std::invalid_argument);
}
} // namespace
} // namespace atx::server
