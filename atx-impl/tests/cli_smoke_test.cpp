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
// Test 1: --help lists all 8 subcommands
// ---------------------------------------------------------------------------
TEST(AtxImplCli, HelpListsAllSubcommands) {
    std::string out, err;
    int rc = run_dispatch({"atx-impl", "--help"}, out, err);

    EXPECT_EQ(rc, 0);

    // All 8 subcommands must appear in usage output.
    EXPECT_NE(out.find("load"),     std::string::npos) << "missing 'load'";
    EXPECT_NE(out.find("panel"),    std::string::npos) << "missing 'panel'";
    EXPECT_NE(out.find("discover"), std::string::npos) << "missing 'discover'";
    EXPECT_NE(out.find("combine"),  std::string::npos) << "missing 'combine'";
    EXPECT_NE(out.find("optimize"), std::string::npos) << "missing 'optimize'";
    EXPECT_NE(out.find("report"),   std::string::npos) << "missing 'report'";
    EXPECT_NE(out.find("run"),      std::string::npos) << "missing 'run'";
    EXPECT_NE(out.find("regime"),   std::string::npos) << "missing 'regime'";
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
// Test 3: implemented stages return exit code 1 on bad/missing args.
//
// S6 implements the last two stubs (report + run). All stages are now
// implemented, so this test is re-pointed to verify that a known-implemented
// stage (load) returns nonzero + an error message when required flags are
// missing. The former "stub returns not-implemented" sub-case is removed;
// there are no remaining stubs to target.
// ---------------------------------------------------------------------------
TEST(AtxImplCli, StageErrorsExitNonzero) {
    // load is implemented — missing --min-date causes InvalidArgument -> exit 1.
    std::string out, err;
    int rc = run_dispatch({"atx-impl", "load", "--zip", "x", "--out", "y"},
                          out, err);
    EXPECT_EQ(rc, 1);
    EXPECT_FALSE(err.empty()) << "expected error message on stderr";
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

// ---------------------------------------------------------------------------
// Test 6: CLI explicit value wins over a file value, even when it is 0.0.
// Mirrors the dispatch run-mode merge: parse_args (CLI base) then
// merge_config_file (file fills only the gaps the CLI left unset).
// ---------------------------------------------------------------------------
TEST(AtxImplCli, CliOverridesFileOnExplicitZero) {
    namespace fs = std::filesystem;
    const fs::path tmp_path =
        fs::temp_directory_path() / "atx_impl_CliOverridesFileOnExplicitZero.cfg";
    {
        std::ofstream f(tmp_path);
        ASSERT_TRUE(f.is_open()) << "failed to open temp config: " << tmp_path;
        f << "gross=1.0\n";
    }

    // run optimize --config <file> --gross 0.0   (explicit CLI zero must win)
    const std::string path_str = tmp_path.string();
    const std::vector<const char*> argv = {
        "atx-impl", "optimize",
        "--config", path_str.c_str(),
        "--gross",  "0.0",
    };
    auto cli_result = atx::impl::parse_args(
        static_cast<int>(argv.size()),
        const_cast<char**>(argv.data()));
    ASSERT_TRUE(cli_result.has_value()) << cli_result.error().message();

    atx::impl::RunConfig cfg = *cli_result;
    auto merge_result = atx::impl::merge_config_file(cfg, path_str);
    ASSERT_TRUE(merge_result.has_value()) << merge_result.error().message();

    EXPECT_DOUBLE_EQ(cfg.gross, 0.0) << "explicit CLI --gross 0.0 must win over file gross=1.0";

    std::error_code ec;
    fs::remove(tmp_path, ec);
}

// ---------------------------------------------------------------------------
// Test 7: file supplies the value when the CLI omits the flag.
// ---------------------------------------------------------------------------
TEST(AtxImplCli, FileSuppliesValueWhenCliOmits) {
    namespace fs = std::filesystem;
    const fs::path tmp_path =
        fs::temp_directory_path() / "atx_impl_FileSuppliesValueWhenCliOmits.cfg";
    {
        std::ofstream f(tmp_path);
        ASSERT_TRUE(f.is_open()) << "failed to open temp config: " << tmp_path;
        f << "gross=1.0\n";
    }

    // run optimize --config <file>   (no --gross on the CLI: file fills it)
    const std::string path_str = tmp_path.string();
    const std::vector<const char*> argv = {
        "atx-impl", "optimize",
        "--config", path_str.c_str(),
    };
    auto cli_result = atx::impl::parse_args(
        static_cast<int>(argv.size()),
        const_cast<char**>(argv.data()));
    ASSERT_TRUE(cli_result.has_value()) << cli_result.error().message();

    atx::impl::RunConfig cfg = *cli_result;
    auto merge_result = atx::impl::merge_config_file(cfg, path_str);
    ASSERT_TRUE(merge_result.has_value()) << merge_result.error().message();

    EXPECT_DOUBLE_EQ(cfg.gross, 1.0) << "file gross=1.0 must fill the gap when CLI omits --gross";

    std::error_code ec;
    fs::remove(tmp_path, ec);
}

// ---------------------------------------------------------------------------
// Test 8: regime-related CLI flags parse correctly
// ---------------------------------------------------------------------------
TEST(Config, ParsesRegimeFlags) {
  const char* argv[] = {"atx", "panel", "--regime-segs", "r.seg",
                        "--regime-fields", "vix,t10y2y"};
  auto cfg = atx::impl::parse_args(6, const_cast<char**>(argv));
  ASSERT_TRUE(cfg.has_value()) << (cfg ? "" : cfg.error().message());
  EXPECT_EQ(cfg.value().regime_segs, "r.seg");
  EXPECT_EQ(cfg.value().regime_fields, "vix,t10y2y");
}

TEST(Config, AcceptsRegimeSubcommand) {
  const char* argv[] = {"atx", "regime", "--staging-dir", "d", "--regime-out", "o.seg"};
  auto cfg = atx::impl::parse_args(6, const_cast<char**>(argv));
  ASSERT_TRUE(cfg.has_value()) << (cfg ? "" : cfg.error().message());
  EXPECT_EQ(cfg.value().subcommand, "regime");
  EXPECT_EQ(cfg.value().staging_dir, "d");
  EXPECT_EQ(cfg.value().regime_out, "o.seg");
}

// ---------------------------------------------------------------------------
// Test 9: P2b — --oos-fraction and --oos-embargo are parsed from CLI and from
// a config file (round-trip).
// ---------------------------------------------------------------------------
TEST(Config, ConfigParsesOosFlags) {
    // CLI parse.
    const char* argv[] = {
        "atx", "discover",
        "--oos-fraction", "0.2",
        "--oos-embargo",  "0.01",
    };
    auto cfg = atx::impl::parse_args(6, const_cast<char**>(argv));
    ASSERT_TRUE(cfg.has_value()) << (cfg ? "" : cfg.error().message());
    EXPECT_DOUBLE_EQ(cfg.value().oos_fraction, 0.2);
    EXPECT_DOUBLE_EQ(cfg.value().oos_embargo,  0.01);

    // Config-file round-trip: write the same flags to a file and parse.
    namespace fs = std::filesystem;
    const fs::path tmp_path =
        fs::temp_directory_path() / "atx_impl_ConfigParsesOosFlags.cfg";
    {
        std::ofstream f(tmp_path);
        ASSERT_TRUE(f.is_open()) << "failed to open temp config: " << tmp_path;
        f << "oos-fraction=0.2\n";
        f << "oos-embargo=0.01\n";
    }
    auto fcfg = atx::impl::parse_config_file(tmp_path.string(), "discover");
    ASSERT_TRUE(fcfg.has_value()) << (fcfg ? "" : fcfg.error().message());
    EXPECT_DOUBLE_EQ(fcfg.value().oos_fraction, 0.2);
    EXPECT_DOUBLE_EQ(fcfg.value().oos_embargo,  0.01);

    std::error_code ec;
    fs::remove(tmp_path, ec);
}

TEST(AtxImplCli, RegimeStageBuildsSegment) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "atx_impl_regime_smoke";
  fs::remove_all(dir);
  fs::create_directories(dir);
  std::ofstream(dir / "dgs2.csv",  std::ios::binary) << "DATE,VALUE\n2020-01-02,1.0\n2020-01-06,1.5\n";
  std::ofstream(dir / "dgs10.csv", std::ios::binary) << "DATE,VALUE\n2020-01-02,2.0\n2020-01-06,2.5\n";
  const std::string out = (dir / "regime.seg").string();
  std::string o, e;
  int rc = run_dispatch({"atx-impl", "regime", "--staging-dir", dir.string(),
                         "--regime-out", out}, o, e);
  EXPECT_EQ(rc, 0) << e;
  EXPECT_TRUE(fs::exists(out)) << "regime.seg not written";
  EXPECT_NE(o.find("stage=regime"), std::string::npos) << o;
}
