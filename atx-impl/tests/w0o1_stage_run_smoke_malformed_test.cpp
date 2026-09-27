// w0o1_stage_run_smoke_malformed_test.cpp — W0-O1 regression for the
// StageRunSyntheticSmoke "invalid stod argument" failure.
//
// Root cause: run_report's StageResult carries two identity kvs (research_artifact_id,
// books_artifact_id). For an unidentified legacy panel (allow_unidentified_panels, the
// smoke fixture) their value is the word "unknown". The smoke test fed every report kv
// to std::stod, which throws std::invalid_argument on "unknown" and aborted the test
// body. The fix (stage_run_synthetic_smoke_test.cpp) audits kvs through
// w0o1_report_kv.hpp: identity kvs are checked as ids, every other kv must parse
// completely and be finite, and malformed input becomes a named failure, never a throw.
// These tests feed that audit the malformed inputs directly.

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "w0o1_report_kv.hpp"

namespace atx_test_w0_o1_stage_run_smoke_malformed {

namespace kv = atx_test_w0_o1_report_kv;
using Kvs = std::vector<std::pair<std::string, std::string>>;

// A valid 64-char lowercase hex artifact id that starts with a letter (std::stod would
// also throw on it).
const std::string kHexId(64U, 'a');

// Pins the original failure mechanism: the report's identity value is not a number.
TEST(ImplStageRunSmokeMalformed_Parse, StodThrowsOnTheReportIdentityValue) {
    EXPECT_THROW((void)std::stod(std::string("unknown")), std::invalid_argument);
    EXPECT_THROW((void)std::stod(kHexId), std::invalid_argument);
}

TEST(ImplStageRunSmokeMalformed_Parse, StrictParseRejectsMalformedWithoutThrowing) {
    const std::vector<std::string> malformed = {
        "unknown", "", "abc", "1.5x", " 1.0", "1.0 ", "1,5", "--1", kHexId, "0x1p3"};
    for (const std::string &v : malformed) {
        EXPECT_NO_THROW({
            const auto r = kv::parse_numeric_kv(v);
            EXPECT_FALSE(r.has_value()) << "'" << v << "' must not parse";
        });
    }
}

TEST(ImplStageRunSmokeMalformed_Parse, StrictParseAcceptsReportNumberSpellings) {
    // std::to_string(double) spellings as run_report emits them, plus scientific.
    const std::vector<std::pair<std::string, double>> ok = {
        {"80", 80.0},
        {"0.000000", 0.0},
        {"-0.123456", -0.123456},
        {"1000000.000000", 1.0e6},
        {"2.5e-3", 2.5e-3},
    };
    for (const auto &[text, want] : ok) {
        const auto r = kv::parse_numeric_kv(text);
        ASSERT_TRUE(r.has_value()) << text;
        EXPECT_DOUBLE_EQ(*r, want) << text;
    }
}

// The shape of the real report stage output for an unidentified panel: identity kvs
// "unknown" first, then numeric kvs. Nothing throws, nothing fails.
TEST(ImplStageRunSmokeMalformed_Audit, UnknownIdentityKvsAreIdentitiesNotNumbers) {
    const Kvs kvs = {{"research_artifact_id", "unknown"},
                     {"books_artifact_id", kHexId},
                     {"periods", "80"},
                     {"portfolio_sharpe", "0.512345"},
                     {"total_pnl_borrow", "12.500000"}};
    kv::KvAudit a;
    EXPECT_NO_THROW(a = kv::audit_report_kvs(kvs));
    EXPECT_EQ(a.identity_checked, 2U);
    EXPECT_EQ(a.numeric_checked, 3U);
    EXPECT_EQ(a.failures, std::vector<std::string>{});
}

// Every malformed kind is reported by name, one failure per kv, and nothing throws.
TEST(ImplStageRunSmokeMalformed_Audit, MalformedKvsAreNamedFailuresNotExceptions) {
    const Kvs kvs = {{"research_artifact_id", "unkown"},    // misspelt identity
                     {"books_artifact_id", "ABCDEF"},       // not 64 lowercase hex
                     {"portfolio_sharpe", "unknown"},       // word in a numeric kv
                     {"pnl_net", "nan"},                    // parses, not finite
                     {"final_equity", "inf"},               // parses, not finite
                     {"avg_gross", "1.5x"},                 // trailing junk
                     {"periods", ""},                       // empty
                     {"capacity_point_aum", "1000.000000"}}; // the one good kv
    kv::KvAudit a;
    EXPECT_NO_THROW(a = kv::audit_report_kvs(kvs));
    EXPECT_EQ(a.identity_checked, 0U);
    EXPECT_EQ(a.numeric_checked, 1U);
    ASSERT_EQ(a.failures.size(), 7U);
    EXPECT_NE(a.failures[0].find("research_artifact_id"), std::string::npos);
    EXPECT_NE(a.failures[1].find("books_artifact_id"), std::string::npos);
    EXPECT_NE(a.failures[2].find("does not parse"), std::string::npos);
    EXPECT_NE(a.failures[3].find("not finite"), std::string::npos);
    EXPECT_NE(a.failures[4].find("not finite"), std::string::npos);
    EXPECT_NE(a.failures[5].find("does not parse"), std::string::npos);
    EXPECT_NE(a.failures[6].find("does not parse"), std::string::npos);
}

TEST(ImplStageRunSmokeMalformed_Audit, IdentityKeySetMatchesTheReportStage) {
    EXPECT_TRUE(kv::is_identity_key("research_artifact_id"));
    EXPECT_TRUE(kv::is_identity_key("books_artifact_id"));
    EXPECT_FALSE(kv::is_identity_key("portfolio_sharpe"));
    EXPECT_TRUE(kv::identity_value_valid("unknown"));
    EXPECT_TRUE(kv::identity_value_valid(kHexId));
    EXPECT_FALSE(kv::identity_value_valid(std::string(63U, 'a')));
    EXPECT_FALSE(kv::identity_value_valid(std::string(64U, 'g')));
}

} // namespace atx_test_w0_o1_stage_run_smoke_malformed
