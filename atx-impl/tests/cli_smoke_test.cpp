#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "config.hpp"
#include "dispatch.hpp"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build a null-terminated argv from a vector of strings and call dispatch.
static int run_dispatch(const std::vector<std::string>& args,
                        std::string& out_str, std::string& err_str) {
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (const auto& a : args) argv.push_back(a.c_str());

    std::ostringstream out_ss, err_ss;
    int rc = atx::impl::dispatch(static_cast<int>(argv.size()),
                                 const_cast<char**>(argv.data()),
                                 out_ss, err_ss);
    out_str = out_ss.str();
    err_str = err_ss.str();
    return rc;
}

// ---------------------------------------------------------------------------
// Test 1: --help lists all 7 subcommands
// ---------------------------------------------------------------------------
TEST(AtxImplCli, HelpListsAllSubcommands) {
    std::string out, err;
    int rc = run_dispatch({"atx-impl", "--help"}, out, err);

    EXPECT_EQ(rc, 0);

    // All 7 subcommands must appear in usage output.
    EXPECT_NE(out.find("load"),     std::string::npos) << "missing 'load'";
    EXPECT_NE(out.find("panel"),    std::string::npos) << "missing 'panel'";
    EXPECT_NE(out.find("discover"), std::string::npos) << "missing 'discover'";
    EXPECT_NE(out.find("combine"),  std::string::npos) << "missing 'combine'";
    EXPECT_NE(out.find("optimize"), std::string::npos) << "missing 'optimize'";
    EXPECT_NE(out.find("report"),   std::string::npos) << "missing 'report'";
    EXPECT_NE(out.find("run"),      std::string::npos) << "missing 'run'";
}

// ---------------------------------------------------------------------------
// Test 2: unknown subcommand returns nonzero + non-empty err
// ---------------------------------------------------------------------------
TEST(AtxImplCli, UnknownSubcommandFails) {
    std::string out, err;
    int rc = run_dispatch({"atx-impl", "frobnicate"}, out, err);

    EXPECT_NE(rc, 0);
    EXPECT_FALSE(err.empty()) << "expected error message on stderr";
}

// ---------------------------------------------------------------------------
// Test 3: stub stage returns exit code 1 and err contains "not implemented"
// ---------------------------------------------------------------------------
TEST(AtxImplCli, UnimplementedStageExitsNonzero) {
    std::string out, err;
    int rc = run_dispatch({"atx-impl", "load", "--zip", "x", "--out", "y"},
                          out, err);

    EXPECT_EQ(rc, 1);
    EXPECT_NE(err.find("not implemented"), std::string::npos)
        << "expected 'not implemented' in stderr, got: '" << err << "'";
}

// ---------------------------------------------------------------------------
// Test 4: config-file parse produces same RunConfig as equivalent CLI flags
// ---------------------------------------------------------------------------
TEST(AtxImplCli, ConfigFileMatchesCliFlags) {
    // Write a temp config file for the 'optimize' subcommand.
    namespace fs = std::filesystem;
    const fs::path tmp_path =
        fs::temp_directory_path() / "atx_impl_ConfigFileMatchesCliFlags.cfg";

    {
        std::ofstream f(tmp_path);
        ASSERT_TRUE(f.is_open()) << "failed to open temp config: " << tmp_path;
        f << "# atx-impl optimize config\n";
        f << "combo=my_combo.parquet\n";
        f << "books-out=books.parquet\n";
        f << "risk-aversion=0.5\n";
        f << "turnover-penalty=0.1\n";
        f << "gross=1.0\n";
        f << "name-cap=0.05\n";
        f << "rebalance=daily\n";
    }

    // Parse from CLI.
    const std::vector<const char*> cli_argv = {
        "atx-impl", "optimize",
        "--combo",            "my_combo.parquet",
        "--books-out",        "books.parquet",
        "--risk-aversion",    "0.5",
        "--turnover-penalty", "0.1",
        "--gross",            "1.0",
        "--name-cap",         "0.05",
        "--rebalance",        "daily",
    };
    auto cli_result = atx::impl::parse_args(
        static_cast<int>(cli_argv.size()),
        const_cast<char**>(cli_argv.data()));
    ASSERT_TRUE(cli_result.has_value()) << cli_result.error().message();

    // Parse from file.
    auto file_result = atx::impl::parse_config_file(
        tmp_path.string(), "optimize");
    ASSERT_TRUE(file_result.has_value()) << file_result.error().message();

    const auto& cli = *cli_result;
    const auto& fil = *file_result;

    EXPECT_EQ(cli.subcommand,       fil.subcommand);
    EXPECT_EQ(cli.combo,            fil.combo);
    EXPECT_EQ(cli.books_out,        fil.books_out);
    EXPECT_DOUBLE_EQ(cli.risk_aversion,    fil.risk_aversion);
    EXPECT_DOUBLE_EQ(cli.turnover_penalty, fil.turnover_penalty);
    EXPECT_DOUBLE_EQ(cli.gross,            fil.gross);
    EXPECT_DOUBLE_EQ(cli.name_cap,         fil.name_cap);
    EXPECT_EQ(cli.rebalance,        fil.rebalance);

    // Clean up.
    std::error_code ec;
    fs::remove(tmp_path, ec);
}

// ---------------------------------------------------------------------------
// Test 5: emit_digest_line format
// ---------------------------------------------------------------------------
TEST(AtxImplCli, DigestLineFormat) {
    std::ostringstream oss;
    atx::impl::emit_digest_line(oss, "load", 0x1ULL, {{"rows", "3"}});

    const std::string expected =
        "[atx-impl] stage=load digest=0000000000000001 rows=3\n";
    EXPECT_EQ(oss.str(), expected)
        << "got: '" << oss.str() << "'";
}
