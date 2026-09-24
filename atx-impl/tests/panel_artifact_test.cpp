#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "panel_artifact.hpp"
#include "artifacts.hpp"
#include "serialize_panel.hpp"
#include "atx/core/sha256.hpp"
#include "atx/engine/alpha/panel.hpp"

namespace {
namespace fs = std::filesystem;
using atx::engine::alpha::Panel;
using atx::impl::PanelIdentity;

[[nodiscard]] Panel artifact_panel() {
    return Panel::create(2, 2, {"close", "volume"},
        {{10.0, std::numeric_limits<atx::f64>::quiet_NaN(), 11.0, 25.0},
         {100.0, 0.0, 110.0, 250.0}}, {1, 0, 1, 1}).value();
}

[[nodiscard]] PanelIdentity artifact_identity() {
    return {std::string(atx::impl::kSpiderRockSecurityIdNamespace),
            {9'007'199'254'740'992LL, 9'007'199'254'740'993LL}, {"7", "9223372036854775807"},
            {3, 11}, "price_basis=total_return_ohlc\nvintage=unknown\n",
            {{"source", atx::core::sha256_hex("source bytes").value()}}};
}

[[nodiscard]] std::string bytes(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void overwrite(const fs::path &path, const std::string &value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    output.close();
    ASSERT_TRUE(output.good());
}

void replace_once(std::string &text, const std::string &from, const std::string &to) {
    const auto position = text.find(from);
    ASSERT_NE(position, std::string::npos);
    text.replace(position, from.size(), to);
}

class PanelArtifactTest : public ::testing::Test {
protected:
    fs::path root;

    void SetUp() override {
        root = fs::temp_directory_path() /
               (std::string("atx_panel_artifact_") +
                ::testing::UnitTest::GetInstance()->current_test_info()->name() + "_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_TRUE(fs::create_directory(root));
    }

    [[nodiscard]] std::string path(const std::string &name = "panel.bin") const {
        return (root / name).string();
    }
};
} // namespace

TEST_F(PanelArtifactTest, RoundTripPreservesApnlBytesExactAxesAndDeterministicIdentity) {
    const auto panel = artifact_panel();
    auto identity = artifact_identity();
    identity.parents.push_back({"earlier", atx::core::sha256_hex("earlier source").value()});
    const auto written = atx::impl::write_panel_artifact(panel, path(), identity);
    ASSERT_TRUE(written.has_value()) << written.error().to_string();
    const auto legacy = atx::impl::write_panel(panel, path("legacy.bin"));
    ASSERT_TRUE(legacy.has_value());
    EXPECT_EQ(bytes(path()), bytes(path("legacy.bin")));
    EXPECT_EQ(written->payload_digest, *legacy);
    EXPECT_EQ(written->payload_sha256, atx::core::sha256_file(path()).value());

    const auto read = atx::impl::read_panel_artifact(path());
    ASSERT_TRUE(read.has_value()) << read.error().to_string();
    EXPECT_EQ(read->artifact_id, written->artifact_id);
    EXPECT_EQ(read->identity.session_keys, identity.session_keys);
    EXPECT_EQ(read->identity.instrument_ids, identity.instrument_ids);
    EXPECT_EQ(read->identity.original_instrument_indices, identity.original_instrument_indices);
    EXPECT_EQ(read->identity.recipe, identity.recipe);
    EXPECT_EQ(read->panel.field_name(atx::usize{0}), "close");
    EXPECT_FALSE(read->panel.in_universe(0, 1));
    const auto manifest = bytes(path() + ".manifest.json");
    EXPECT_NE(manifest.find("\"9007199254740993\""), std::string::npos);
    EXPECT_NE(manifest.find("session-label-not-availability"), std::string::npos);

    std::reverse(identity.parents.begin(), identity.parents.end());
    const auto repeated = atx::impl::write_panel_artifact(panel, path("again.bin"), identity);
    ASSERT_TRUE(repeated.has_value());
    EXPECT_EQ(repeated->artifact_id, written->artifact_id);
    EXPECT_EQ(bytes(path("again.bin") + ".manifest.json"), manifest);
}

TEST_F(PanelArtifactTest, EveryIdentityComponentChangesTheArtifactDespiteIdenticalPayload) {
    const auto panel = artifact_panel();
    const auto base = artifact_identity();
    const auto written = atx::impl::write_panel_artifact(panel, path(), base);
    ASSERT_TRUE(written.has_value());
    std::vector<PanelIdentity> variants(6, base);
    variants[0].session_keys[1] += 1;
    std::swap(variants[1].instrument_ids[0], variants[1].instrument_ids[1]);
    variants[2].original_instrument_indices[0] = 5;
    variants[3].instrument_namespace = "another.source";
    variants[4].recipe += "warmup=21\n";
    variants[5].parents[0].sha256 = atx::core::sha256_hex("other source").value();
    for (atx::usize i = 0; i < variants.size(); ++i) {
        const auto changed = atx::impl::write_panel_artifact(
            panel, path("variant" + std::to_string(i) + ".bin"), variants[i]);
        ASSERT_TRUE(changed.has_value()) << changed.error().to_string();
        EXPECT_EQ(changed->payload_sha256, written->payload_sha256);
        EXPECT_NE(changed->artifact_id, written->artifact_id);
        EXPECT_EQ(atx::impl::require_same_panel_axes(base, variants[i]).has_value(), i >= 4);
    }
    EXPECT_TRUE(atx::impl::require_panel_parent(base, "source", base.parents[0].sha256));
    EXPECT_FALSE(atx::impl::require_panel_parent(base, "other", base.parents[0].sha256));
    EXPECT_FALSE(atx::impl::require_panel_parent(base, "source", variants[5].parents[0].sha256));
}

TEST_F(PanelArtifactTest, MissingAndTamperedManifestsNeverFallBackToLegacyPayload) {
    ASSERT_TRUE(atx::impl::write_panel(artifact_panel(), path("legacy.bin")));
    EXPECT_FALSE(atx::impl::read_panel_artifact(path("legacy.bin")));
    ASSERT_TRUE(atx::impl::write_panel_artifact(artifact_panel(), path(), artifact_identity()));
    const auto original = bytes(path() + ".manifest.json");
    const std::vector<std::pair<std::string, std::string>> changes{
        {"vintage=unknown", "vintage=known"},
        {"\"schema_version\": 1", "\"schema_version\": 2"},
        {"\"9007199254740993\"", "9007199254740993"},
        {"session-label-not-availability", "publication-time"},
        {"\"source\"", "\"different-parent-role\""}};
    for (const auto &[from, to] : changes) {
        auto altered = original;
        replace_once(altered, from, to);
        overwrite(path() + ".manifest.json", altered);
        EXPECT_FALSE(atx::impl::read_panel_artifact(path())) << from;
    }
    // Even duplicate keys containing equal values must not silently overwrite.
    auto duplicate = original;
    duplicate.insert(1, "\"schema\":\"atx.panel-artifact\",");
    overwrite(path() + ".manifest.json", duplicate);
    const auto rejected = atx::impl::read_panel_artifact(path());
    ASSERT_FALSE(rejected.has_value());
    EXPECT_NE(rejected.error().message().find("duplicate"), std::string::npos);
}

TEST_F(PanelArtifactTest, PayloadTamperingAndOversizedHeaderAreRejectedBeforeNumericRead) {
    ASSERT_TRUE(atx::impl::write_panel_artifact(artifact_panel(), path(), artifact_identity()));
    const auto original = bytes(path());
    auto changed = original;
    changed[changed.size() - 9] ^= 1; // Universe mask; size and header still match.
    overwrite(path(), changed);
    const auto bad_hash = atx::impl::read_panel_artifact(path());
    ASSERT_FALSE(bad_hash.has_value());
    EXPECT_NE(bad_hash.error().message().find("SHA-256"), std::string::npos);
    changed = original;
    std::fill(changed.begin() + 8, changed.begin() + 16, static_cast<char>(0xff));
    overwrite(path(), changed);
    const auto bad_shape = atx::impl::read_panel_artifact(path());
    ASSERT_FALSE(bad_shape.has_value());
    EXPECT_NE(bad_shape.error().message().find("shape/header"), std::string::npos);
}

TEST_F(PanelArtifactTest, InvalidAxesSourceIdsAndParentRolesFailBeforePublication) {
    const auto base = artifact_identity();
    std::vector<PanelIdentity> invalid(10, base);
    invalid[0].session_keys[1] = invalid[0].session_keys[0];
    invalid[1].instrument_ids[0] = "0007";
    invalid[2].instrument_ids[0] = "0";
    invalid[3].instrument_ids[0] = "9223372036854775808";
    invalid[4].instrument_ids[0] = invalid[4].instrument_ids[1];
    invalid[5].original_instrument_indices.clear();
    invalid[6].original_instrument_indices[0] = invalid[6].original_instrument_indices[1];
    invalid[7].parents.push_back(invalid[7].parents[0]);
    invalid[8].parents[0].sha256 = "bad-hash";
    invalid[9].recipe.clear();
    for (atx::usize i = 0; i < invalid.size(); ++i) {
        const auto candidate = path("bad" + std::to_string(i) + ".bin");
        EXPECT_FALSE(atx::impl::write_panel_artifact(artifact_panel(), candidate, invalid[i]));
        EXPECT_FALSE(fs::exists(candidate));
        EXPECT_FALSE(fs::exists(candidate + ".manifest.json"));
    }
}

TEST_F(PanelArtifactTest, ExistingAndInterruptedOutputsCannotBeOverwritten) {
    const auto panel = artifact_panel();
    const auto identity = artifact_identity();
    ASSERT_TRUE(atx::impl::write_panel_artifact(panel, path(), identity));
    const auto payload = bytes(path());
    const auto manifest = bytes(path() + ".manifest.json");
    EXPECT_FALSE(atx::impl::write_panel_artifact(panel, path(), identity));
    EXPECT_EQ(bytes(path()), payload);
    EXPECT_EQ(bytes(path() + ".manifest.json"), manifest);

    overwrite(path("partial.bin") + ".partial", "interrupted payload");
    EXPECT_FALSE(atx::impl::write_panel_artifact(panel, path("partial.bin"), identity));
    EXPECT_EQ(bytes(path("partial.bin") + ".partial"), "interrupted payload");
    EXPECT_FALSE(fs::exists(path("partial.bin")));
    EXPECT_FALSE(fs::exists(path("partial.bin") + ".manifest.json"));
    fs::create_directory(path("reserved.bin") + ".publishing");
    EXPECT_FALSE(atx::impl::write_panel_artifact(panel, path("reserved.bin"), identity));
}

TEST_F(PanelArtifactTest, ExactSignedDateBoundariesAndDifferentShapesAreChecked) {
    auto identity = artifact_identity();
    identity.session_keys = {std::numeric_limits<atx::i64>::min(),
                             std::numeric_limits<atx::i64>::max()};
    ASSERT_TRUE(atx::impl::write_panel_artifact(artifact_panel(), path(), identity));
    const auto read = atx::impl::read_panel_artifact(path());
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->identity.session_keys, identity.session_keys);
    identity.instrument_ids.pop_back();
    identity.original_instrument_indices.pop_back();
    EXPECT_FALSE(atx::impl::write_panel_artifact(artifact_panel(), path("shape.bin"), identity));
}

TEST_F(PanelArtifactTest, ExcessiveManifestSizeAndDepthAreRejected) {
    const std::string too_large(16U * 1024U * 1024U + 1U, ' ');
    overwrite(path() + ".manifest.json", too_large);
    const auto size = atx::impl::read_panel_artifact(path());
    ASSERT_FALSE(size.has_value());
    EXPECT_NE(size.error().message().find("byte size"), std::string::npos);
    overwrite(path() + ".manifest.json", std::string(32, '[') + "0" + std::string(32, ']'));
    const auto depth = atx::impl::read_panel_artifact(path());
    ASSERT_FALSE(depth.has_value());
    EXPECT_NE(depth.error().message().find("nesting"), std::string::npos);
}

TEST_F(PanelArtifactTest, SnapshotDecodeRemainsBoundToCapturedBytesAfterSourceReplacement) {
    const auto written = atx::impl::write_panel_artifact(
        artifact_panel(), path(), artifact_identity());
    ASSERT_TRUE(written.has_value());
    const auto original = bytes(path());
    std::vector<atx::u8> snapshot(original.begin(), original.end());
    const auto captured_hash = atx::core::sha256_hex(std::as_bytes(std::span(snapshot)));
    ASSERT_TRUE(captured_hash.has_value());
    EXPECT_EQ(*captured_hash, written->payload_sha256);

    // Replace the path with another valid APNL (including its own valid FNV).
    // The decoder must consume the verified old snapshot, never the changed path.
    const auto replacement = Panel::create(2, 2, {"close", "volume"},
        {{999.0, 998.0, 997.0, 996.0}, {1.0, 2.0, 3.0, 4.0}}, {1, 1, 1, 1});
    ASSERT_TRUE(replacement.has_value());
    ASSERT_TRUE(atx::impl::write_panel(*replacement, path()).has_value());
    auto captured = atx::impl::read_panel_bytes(snapshot);
    ASSERT_TRUE(captured.has_value()) << captured.error().message();
    const auto close = captured->field_id("close");
    ASSERT_TRUE(close.has_value());
    EXPECT_DOUBLE_EQ(captured->field_all(*close)[0], 10.0);
    EXPECT_FALSE(captured->in_universe(0, 1));
    const auto current = atx::impl::read_panel(path());
    ASSERT_TRUE(current.has_value());
    EXPECT_DOUBLE_EQ(current->field_all(*close)[0], 999.0);
    EXPECT_FALSE(atx::impl::read_panel_artifact(path()).has_value());

    // Decoded columns are owned, so subsequent destruction/mutation of the input
    // buffer cannot change the returned panel. Restoring the original path again
    // gives a valid identified artifact without any stale second read in decode.
    std::fill(snapshot.begin(), snapshot.end(), atx::u8{0});
    EXPECT_DOUBLE_EQ(captured->field_all(*close)[0], 10.0);
    EXPECT_FALSE(captured->in_universe(0, 1));
    overwrite(path(), original);
    const auto restored = atx::impl::read_panel_artifact(path());
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored->artifact_id, written->artifact_id);
}

TEST_F(PanelArtifactTest, SnapshotShapeBoundsHoldEvenWhenMalformedBytesHaveAValidFnv) {
    ASSERT_TRUE(atx::impl::write_panel(artifact_panel(), path()).has_value());
    const auto original = bytes(path());
    const std::vector<atx::u8> valid(original.begin(), original.end());
    auto repair_digest = [](std::vector<atx::u8> &buffer) {
        const auto payload_size = buffer.size() - 8U;
        const auto digest = atx::impl::fnv1a64(buffer.data(), payload_size);
        for (atx::usize i = 0; i < 8; ++i) {
            buffer[payload_size + i] = static_cast<atx::u8>((digest >> (8U * i)) & 0xffU);
        }
    };
    std::vector<std::vector<atx::u8>> malformed(4, valid);
    // dates * instruments overflows, field count is unallocatable, a name spills
    // beyond the payload, and otherwise-valid data contains an extra byte.
    std::fill(malformed[0].begin() + 8, malformed[0].begin() + 16, atx::u8{0xff});
    std::fill(malformed[1].begin() + 24, malformed[1].begin() + 32, atx::u8{0xff});
    std::fill(malformed[2].begin() + 32, malformed[2].begin() + 36, atx::u8{0xff});
    malformed[3].insert(malformed[3].end() - 8, atx::u8{0});
    for (auto &candidate : malformed) {
        repair_digest(candidate);
        EXPECT_NO_THROW({
            const auto result = atx::impl::read_panel_bytes(candidate);
            EXPECT_FALSE(result.has_value());
        });
    }
    auto corrupt = valid;
    corrupt.back() ^= 1U;
    EXPECT_FALSE(atx::impl::read_panel_bytes(corrupt).has_value());
    EXPECT_FALSE(atx::impl::read_panel_bytes(std::span(valid).first(39)).has_value());
}

TEST_F(PanelArtifactTest, PayloadBudgetRejectsLargerReplacementsBeforeSnapshotRead) {
    const auto written = atx::impl::write_panel_artifact(
        artifact_panel(), path(), artifact_identity());
    ASSERT_TRUE(written.has_value());
    const auto original_manifest = bytes(path() + ".manifest.json");
    const auto cap = static_cast<atx::u64>(fs::file_size(path()));
    const auto exact = atx::impl::read_panel_artifact(path(), cap);
    ASSERT_TRUE(exact.has_value()) << exact.error().message();
    EXPECT_EQ(exact->artifact_id, written->artifact_id);
    EXPECT_EQ(exact->payload_sha256, written->payload_sha256);
    for (const auto limit : {atx::u64{0}, cap - 1U}) {
        const auto too_small = atx::impl::read_panel_artifact(path(), limit);
        ASSERT_FALSE(too_small.has_value());
        EXPECT_EQ(too_small.error().code(), atx::core::ErrorCode::OutOfRange);
        EXPECT_NE(too_small.error().message().find("declared payload"), std::string::npos);
    }

    auto larger_identity = artifact_identity();
    larger_identity.session_keys.push_back(larger_identity.session_keys.back() + 1);
    const auto larger_panel = Panel::create(3, 2, {"close", "volume"},
        {{10.0, 20.0, 11.0, 21.0, 12.0, 22.0}, {100.0, 200.0, 110.0, 210.0, 120.0, 220.0}}, {});
    ASSERT_TRUE(larger_panel.has_value());
    const auto larger = atx::impl::write_panel_artifact(
        *larger_panel, path("larger.bin"), larger_identity);
    ASSERT_TRUE(larger.has_value());
    ASSERT_GT(fs::file_size(path("larger.bin")), cap);

    // An unchanged small manifest cannot make an oversized opened file allocate.
    overwrite(path(), bytes(path("larger.bin")));
    EXPECT_EQ(bytes(path() + ".manifest.json"), original_manifest);
    const auto opened_too_large = atx::impl::read_panel_artifact(path(), cap);
    ASSERT_FALSE(opened_too_large.has_value());
    EXPECT_EQ(opened_too_large.error().code(), atx::core::ErrorCode::OutOfRange);
    EXPECT_NE(opened_too_large.error().message().find("opened payload"), std::string::npos);

    // Replacing both files with a valid, same-recipe larger artifact also stays
    // subject to the caller's original cap. The uncapped API remains compatible.
    overwrite(path() + ".manifest.json", bytes(path("larger.bin") + ".manifest.json"));
    const auto declared_too_large = atx::impl::read_panel_artifact(path(), cap);
    ASSERT_FALSE(declared_too_large.has_value());
    EXPECT_EQ(declared_too_large.error().code(), atx::core::ErrorCode::OutOfRange);
    EXPECT_NE(declared_too_large.error().message().find("declared payload"), std::string::npos);
    const auto uncapped = atx::impl::read_panel_artifact(path());
    ASSERT_TRUE(uncapped.has_value()) << uncapped.error().message();
    EXPECT_EQ(uncapped->artifact_id, larger->artifact_id);
    EXPECT_EQ(uncapped->panel.dates(), 3U);
    EXPECT_EQ(uncapped->identity.recipe, exact->identity.recipe);
}
