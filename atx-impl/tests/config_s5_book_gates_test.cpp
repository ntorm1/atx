// config_s5_book_gates_test.cpp — p9 Sprint 5 (S5-0): the book-gate / capacity /
// robustness-battery / borrow CLI flag surface.
//
// Every new RunConfig field defaults to its INERT value, so a run/optimize/report/
// discover invocation with NONE of these flags asserted is byte-identical to pre-S5
// (proven separately by the pinned goldens — this file only proves the PARSE layer:
// each of the 6 flags reaches its field under its canonical dashed key, and a
// negative value on the 3 double flags fails closed with Err(InvalidArgument)).

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "config.hpp"

using atx::impl::parse_args;
using atx::impl::RunConfig;

namespace {

atx::core::Result<RunConfig> parse(std::vector<std::string> args) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& a : args) argv.push_back(a.data());
    return parse_args(static_cast<int>(argv.size()), argv.data());
}

} // namespace

// ---------------------------------------------------------------------------
// ConfigS5BookGates.Defaults_AreInert — a bare invocation leaves every new S5
// field at the byte-identical-to-pre-S5 default.
// ---------------------------------------------------------------------------
TEST(ConfigS5BookGates, Defaults_AreInert) {
    auto r = parse({"atx-impl", "optimize"});
    ASSERT_TRUE(r.has_value());
    const RunConfig& cfg = *r;
    EXPECT_DOUBLE_EQ(cfg.book_turnover_gate, 0.0);
    EXPECT_DOUBLE_EQ(cfg.participation_cap, 0.0);
    EXPECT_DOUBLE_EQ(cfg.borrow_bps, 0.0);
    EXPECT_FALSE(cfg.robustness_sub_universe);
    EXPECT_FALSE(cfg.robustness_alt_neutralization);
    EXPECT_FALSE(cfg.robustness_param_perturb);
}

// ---------------------------------------------------------------------------
// ConfigS5BookGates.Flags_RoundTrip — each of the 6 flags parses to its field
// and is recorded in cfg.set_flags under its canonical dashed key.
// ---------------------------------------------------------------------------
TEST(ConfigS5BookGates, Flags_RoundTrip) {
    auto r = parse({"atx-impl", "optimize",
                    "--book-turnover-gate", "0.20",
                    "--participation-cap", "0.05",
                    "--borrow-bps", "50.0",
                    "--robustness-sub-universe",
                    "--robustness-alt-neutralization",
                    "--robustness-param-perturb"});
    ASSERT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().message());
    const RunConfig& cfg = *r;
    EXPECT_DOUBLE_EQ(cfg.book_turnover_gate, 0.20);
    EXPECT_DOUBLE_EQ(cfg.participation_cap, 0.05);
    EXPECT_DOUBLE_EQ(cfg.borrow_bps, 50.0);
    EXPECT_TRUE(cfg.robustness_sub_universe);
    EXPECT_TRUE(cfg.robustness_alt_neutralization);
    EXPECT_TRUE(cfg.robustness_param_perturb);

    EXPECT_TRUE(cfg.set_flags.count("book-turnover-gate"));
    EXPECT_TRUE(cfg.set_flags.count("participation-cap"));
    EXPECT_TRUE(cfg.set_flags.count("borrow-bps"));
    EXPECT_TRUE(cfg.set_flags.count("robustness-sub-universe"));
    EXPECT_TRUE(cfg.set_flags.count("robustness-alt-neutralization"));
    EXPECT_TRUE(cfg.set_flags.count("robustness-param-perturb"));
}

// ---------------------------------------------------------------------------
// ConfigS5BookGates.NegativeDoublesFailClosed — the 3 double flags reject a
// negative value (fail-loud, mirroring --cost-bps's own non-negative guard).
// ---------------------------------------------------------------------------
TEST(ConfigS5BookGates, NegativeBookTurnoverGateFailsClosed) {
    auto r = parse({"atx-impl", "optimize", "--book-turnover-gate", "-0.1"});
    EXPECT_FALSE(r.has_value());
}

TEST(ConfigS5BookGates, NegativeParticipationCapFailsClosed) {
    auto r = parse({"atx-impl", "optimize", "--participation-cap", "-0.01"});
    EXPECT_FALSE(r.has_value());
}

TEST(ConfigS5BookGates, NegativeBorrowBpsFailsClosed) {
    auto r = parse({"atx-impl", "report", "--borrow-bps", "-5"});
    EXPECT_FALSE(r.has_value());
}

// A zero value is the inert default and must be accepted (0 = off, not invalid).
TEST(ConfigS5BookGates, ZeroDoublesAccepted) {
    auto r = parse({"atx-impl", "optimize",
                    "--book-turnover-gate", "0",
                    "--participation-cap", "0",
                    "--borrow-bps", "0"});
    ASSERT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().message());
    EXPECT_DOUBLE_EQ(r->book_turnover_gate, 0.0);
    EXPECT_DOUBLE_EQ(r->participation_cap, 0.0);
    EXPECT_DOUBLE_EQ(r->borrow_bps, 0.0);
}
