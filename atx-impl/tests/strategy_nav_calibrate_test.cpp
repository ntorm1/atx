// nav --calibrate-gross (P9 C2): Ruling PM6-6's gross matching in one process
// (strategy_nav_calibrate.hpp). The arithmetic against Python's (numpy's pairwise mean, f"{x:.4f}"
// with ties to even, values computed with numpy 1.26.4 / Python 3.12), the step's decision, and
// the verb end to end on the spo CLI fixture: within the tolerance the published directory is the
// plain run at L byte for byte (summary.json gains calibration only); one correction publishes the
// plain run at L' (the two-process result) byte for byte, capacity books and v7 files included;
// L' outside [1, 2] and the usage errors publish nothing.
//
// Suites: CalibrateGross

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "../src/strategy_nav_calibrate.hpp"
#include "../src/strategy_nav_replay.hpp"
#include "strategy_spo_cli_fixture.hpp"
#include "strategy_spo_fixture.hpp"

namespace {
using namespace atx;
using namespace atx::impl::strategy::spo::fixture;
namespace st = atx::impl::strategy;
using Json = nlohmann::json;

u64 bit_pattern(f64 x) { return std::bit_cast<u64>(x); }

std::string bytes_of(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
std::vector<std::string> tree(const std::filesystem::path& root) {
  std::vector<std::string> names;
  for (const auto& e : std::filesystem::recursive_directory_iterator(root))
    if (e.is_regular_file())
      names.push_back(std::filesystem::relative(e.path(), root).generic_string());
  std::sort(names.begin(), names.end());
  return names;
}
// Every file of `a` equals `b`'s byte for byte, except summary.json, which equals b's once
// calibration is dropped; returns the calibration record (null when absent).
Json expect_plain_run_plus_calibration(const std::filesystem::path& a,
                                       const std::filesystem::path& b) {
  const auto names = tree(a);
  EXPECT_EQ(names, tree(b)) << a.string();
  for (const auto& name : names) {
    if (name == "summary.json") continue;
    const auto x = bytes_of(a / name), y = bytes_of(b / name);
    if (x == y) continue;
    // The first differing line of each (synthetic fixture rows; diagnostics only).
    std::stringstream xs(x), ys(y);
    std::string xl, yl;
    usize line = 0;
    while (std::getline(xs, xl) && std::getline(ys, yl) && xl == yl) ++line;
    ADD_FAILURE() << a.string() << ' ' << name << " differs at line " << line << "\n  "
                  << xl.substr(0, 400) << "\n  " << yl.substr(0, 400);
  }
  auto summary = read_json_file(a / "summary.json");
  const Json calibration = summary.contains("calibration") ? summary.at("calibration") : Json();
  summary.erase("calibration");
  EXPECT_EQ(summary, read_json_file(b / "summary.json")) << a.string();
  return calibration;
}
// The primary S2 book's all-rows mean gross of a published directory, from its daily CSV (17
// significant digits: the values round-trip), as nav_summ computes it.
f64 csv_all_rows_gross(const std::filesystem::path& dir) {
  const auto label = read_json_file(dir / "summary.json").at("primary_scenario").get<std::string>();
  std::ifstream in(dir / ("daily_" + label + ".csv"));
  std::string line;
  std::getline(in, line);
  std::vector<std::string> header;
  {
    std::stringstream row(line);
    for (std::string cell; std::getline(row, cell, ',');) header.push_back(cell);
  }
  const auto column = static_cast<usize>(
      std::find(header.begin(), header.end(), "gross_leverage") - header.begin());
  EXPECT_LT(column, header.size());
  std::vector<f64> gross;
  while (std::getline(in, line)) {
    std::stringstream row(line);
    std::string cell;
    for (usize k = 0; k <= column && std::getline(row, cell, ','); ++k) {}
    gross.push_back(std::stod(cell));
  }
  return st::numpy_mean(gross);
}
// The leverage as the -gm spec carries it (Python f"{x:.4f}").
std::string four_decimals(f64 x) {
  std::array<char, 64> text{};
  const auto printed =
      std::to_chars(text.data(), text.data() + text.size(), x, std::chars_format::fixed, 4);
  EXPECT_EQ(printed.ec, std::errc{});
  return std::string(text.data(), printed.ptr);
}

// The CLI run of the fixture: aim-partial-v5 at --aim-leverage `leverage`, plus `extra`.
struct Verb {
  Directory dir;
  Role role{40, 12, 61};
  RunInputs inputs = write_run_inputs(dir.path, role);
  int run(const std::filesystem::path& output, const std::string& leverage,
          std::vector<std::string> extra, std::string* error = nullptr) const {
    std::vector<std::string> args{
        "nav", "--combined", inputs.combined, "--combined-sha256", inputs.combined_sha256,
        "--role", inputs.role, "--role-sha256", inputs.role_sha256,
        "--output", output.string(), "--rule", "aim-partial-v5", "--cadence", "2",
        "--trade-fraction", ".25", "--dust-multiple", ".1", "--aim-leverage", leverage};
    args.insert(args.end(), extra.begin(), extra.end());
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    std::ostringstream out, err;
    const int code = st::dispatch_nav_replay(static_cast<int>(argv.size()), argv.data(), out, err);
    if (error) *error = err.str();
    return code;
  }
};

// numpy's mean (0.0 + the pairwise sum, / n), bit for bit, at sizes on both sides of numpy's
// 8-wide unroll and 128-value block (values from numpy 1.26.4); at 129 and 1023 a running sum
// would differ.
TEST(CalibrateGross, NumpyMeanIsNumpysPairwiseMean) {
  const std::array<std::pair<usize, u64>, 5> expected{{{5U, 0x3ff00837765f0148ULL},
                                                       {100U, 0x3ff0cb5cb1af5fc7ULL},
                                                       {129U, 0x3ff106eecbe02916ULL},
                                                       {1000U, 0x3ff8041bbb2f80a5ULL},
                                                       {1023U, 0x3ff8335aa3d1c806ULL}}};
  for (const auto& [n, bits] : expected) {
    std::vector<f64> values(n);
    for (usize k = 0; k < n; ++k) values[k] = 1.0 + static_cast<f64>(k) / 997.0;
    EXPECT_EQ(bit_pattern(st::numpy_mean(values)), bits) << n;
  }
  EXPECT_TRUE(std::isnan(st::numpy_mean({})));
}

// f"{x:.4f}" parsed back (Python 3.12 values), ties to even; the step keeps L bit for bit
// within the tolerance and refuses a non-positive gross and an L' outside [1, 2].
TEST(CalibrateGross, FourDecimalsAndTheStepArePm66) {
  const std::array<std::pair<f64, u64>, 6> rounded{{{1.03125, 0x3ff07fcb923a29c7ULL},
                                                    {1.23456789, 0x3ff3c0ebedfa43feULL},
                                                    {1.00005, 0x3ff00068db8bac71ULL},
                                                    {1.99995, 0x3fffff972474538fULL},
                                                    {1.03135, 0x3ff080346dc5d639ULL},
                                                    {2.00004, 0x4000000000000000ULL}}};
  for (const auto& [x, bits] : rounded) {
    const auto got = st::leverage_4_decimals(x);
    ASSERT_TRUE(got) << x;
    EXPECT_EQ(bit_pattern(*got), bits) << x;
  }
  EXPECT_FALSE(st::leverage_4_decimals(std::nan("")));
  const auto kept = st::calibrate_step(1.3, 1.0, 1.004, .005);
  ASSERT_TRUE(kept);
  EXPECT_FALSE(kept->corrected);
  EXPECT_EQ(bit_pattern(kept->leverage), bit_pattern(1.3));
  const auto moved = st::calibrate_step(1.3, 1.0, 1.1, .005); // 1.3 / 1.1 = 1.18181...
  ASSERT_TRUE(moved);
  EXPECT_TRUE(moved->corrected);
  EXPECT_EQ(bit_pattern(moved->leverage), bit_pattern(1.1818));
  EXPECT_FALSE(st::calibrate_step(1.3, 1.0, 0.0, .005));
  EXPECT_FALSE(st::calibrate_step(1.3, 1.0, std::nan(""), .005));
  EXPECT_FALSE(st::calibrate_step(1.9, 1.2, 1.0, .005)); // 2.28
  EXPECT_FALSE(st::calibrate_step(1.0, 0.8, 1.0, .005)); // 0.8
  EXPECT_TRUE(st::validate_calibrate(st::NavCalibrateOptions{}));
  EXPECT_FALSE(st::validate_calibrate(st::NavCalibrateOptions{-1.0, .005}));
  EXPECT_FALSE(st::validate_calibrate(st::NavCalibrateOptions{1.0, 0.0}));
}

// Within the tolerance: one calibration pass, no correction, and the published directory is the
// plain run at L byte for byte; summary.json differs by calibration alone, whose G is the plain
// run's all-rows S2 gross (its daily CSV, read as nav_summ reads it).
TEST(CalibrateGross, WithinToleranceOnePass) {
  const Verb verb;
  const auto plain = verb.dir.path / "plain", calibrated = verb.dir.path / "calibrated";
  std::string error;
  ASSERT_EQ(verb.run(plain, "1.5", {}, &error), 0) << error;
  const f64 g = csv_all_rows_gross(plain);
  ASSERT_TRUE(std::isfinite(g) && g > 0) << g;
  const f64 target = g + .004;
  std::ostringstream gross;
  gross.precision(17);
  gross << target;
  ASSERT_EQ(verb.run(calibrated, "1.5", {"--calibrate-gross", gross.str()}, &error), 0) << error;
  const auto calibration = expect_plain_run_plus_calibration(calibrated, plain);
  ASSERT_TRUE(calibration.is_object());
  EXPECT_EQ(calibration.at("corrected"), false);
  EXPECT_EQ(bit_pattern(calibration.at("leverage").get<f64>()), bit_pattern(1.5));
  EXPECT_EQ(bit_pattern(calibration.at("leverage_requested").get<f64>()), bit_pattern(1.5));
  EXPECT_EQ(bit_pattern(calibration.at("gross_requested").get<f64>()), bit_pattern(g));
  EXPECT_EQ(bit_pattern(calibration.at("gross").get<f64>()), bit_pattern(g));
  EXPECT_EQ(bit_pattern(calibration.at("target").get<f64>()), bit_pattern(target));
  EXPECT_EQ(calibration.at("tolerance").get<f64>(), st::nav_calibrate_default_tolerance);
  EXPECT_EQ(calibration.at("matched"), true);
  EXPECT_EQ(calibration.at("construction_reused"), true);
  EXPECT_TRUE(calibration.at("rule").is_string());
}

// Outside the tolerance: ONE correction L' = L x G_P / G (4 decimals) and the published
// directory is the two-process result, the plain run at L' (as the -gm spec writes it), byte for
// byte; matched is |G' - G_P| <= T of that run. Plain and with --capacity-curve (the capacity
// books join pass 2's lockstep only; the v7 files are pass 2's books').
TEST(CalibrateGross, OneCorrectionMatchesTwoProcess) {
  const Verb verb;
  for (const bool capacity : {false, true}) {
    const std::string tag = capacity ? "-capacity" : "";
    const std::vector<std::string> extra =
        capacity ? std::vector<std::string>{"--capacity-curve"} : std::vector<std::string>{};
    const auto first = verb.dir.path / ("first" + tag);
    std::string error;
    ASSERT_EQ(verb.run(first, "1.5", extra, &error), 0) << error;
    const f64 g = csv_all_rows_gross(first);
    const f64 target = g * 1.08;
    const auto corrected = st::leverage_4_decimals(1.5 * target / g);
    ASSERT_TRUE(corrected);
    ASSERT_TRUE(*corrected >= 1.0 && *corrected <= 2.0) << *corrected;
    const auto second = verb.dir.path / ("second" + tag);
    ASSERT_EQ(verb.run(second, four_decimals(*corrected), extra, &error), 0) << error;
    std::ostringstream gross;
    gross.precision(17);
    gross << target;
    // The fixture's gross is small (about .015): a 1e-4 tolerance makes an 8% gap a correction.
    auto args = extra;
    args.insert(args.end(), {"--calibrate-gross", gross.str(), "--calibrate-tolerance", "1e-4"});
    const auto calibrated = verb.dir.path / ("calibrated" + tag);
    ASSERT_EQ(verb.run(calibrated, "1.5", args, &error), 0) << error;
    const auto calibration = expect_plain_run_plus_calibration(calibrated, second);
    ASSERT_TRUE(calibration.is_object()) << tag;
    EXPECT_EQ(calibration.at("corrected"), true) << tag << ' ' << calibration.dump();
    EXPECT_EQ(bit_pattern(calibration.at("leverage").get<f64>()), bit_pattern(*corrected)) << tag;
    EXPECT_EQ(bit_pattern(calibration.at("gross_requested").get<f64>()), bit_pattern(g)) << tag;
    const f64 g2 = csv_all_rows_gross(second);
    EXPECT_EQ(bit_pattern(calibration.at("gross").get<f64>()), bit_pattern(g2)) << tag;
    EXPECT_EQ(calibration.at("matched").get<bool>(), std::abs(g2 - target) <= 1e-4) << tag;
    EXPECT_EQ(calibration.at("tolerance").get<f64>(), 1e-4) << tag;
    // recipe.json is the plain run's at L' (compared above), so the decide path's recipe pin
    // of the -gm argv holds; the calibrated run moved off L.
    EXPECT_FALSE(bytes_of(calibrated / "recipe.json") == bytes_of(first / "recipe.json")) << tag;
    if (capacity) EXPECT_TRUE(std::filesystem::exists(calibrated / "capacity" / "summary.json"));
  }
}

// L' outside [1, 2] is refused before anything is published; so are the usage errors (the
// tolerance alone, a non-finite or non-positive gross or tolerance, a grid, another rule).
TEST(CalibrateGross, RefusesOutsideRange) {
  const Verb verb;
  const auto plain = verb.dir.path / "plain";
  std::string error;
  ASSERT_EQ(verb.run(plain, "1.5", {}, &error), 0) << error;
  const f64 g = csv_all_rows_gross(plain);
  const auto refused = [&](const std::string& name, const std::string& leverage,
                           std::vector<std::string> extra) {
    const auto output = verb.dir.path / name;
    const int code = verb.run(output, leverage, std::move(extra), &error);
    EXPECT_FALSE(std::filesystem::exists(output)) << name;
    return code;
  };
  const auto text = [](f64 x) {
    std::ostringstream s;
    s.precision(17);
    s << x;
    return s.str();
  };
  EXPECT_EQ(refused("above", "1.5",
                    {"--calibrate-gross", text(g * 1.5), "--calibrate-tolerance", "1e-4"}),
            1); // L' = 2.25
  EXPECT_NE(error.find("[1, 2]"), std::string::npos) << error;
  EXPECT_EQ(refused("below", "1.5",
                    {"--calibrate-gross", text(g * 0.6), "--calibrate-tolerance", "1e-4"}),
            1); // L' = 0.9
  EXPECT_EQ(refused("tolerance-alone", "1.5", {"--calibrate-tolerance", ".01"}), 2);
  EXPECT_EQ(refused("nan", "1.5", {"--calibrate-gross", "nan"}), 2);
  EXPECT_EQ(refused("negative", "1.5", {"--calibrate-gross", "-1"}), 1);
  EXPECT_EQ(refused("zero-tolerance", "1.5",
                    {"--calibrate-gross", text(g), "--calibrate-tolerance", "0"}),
            1);
  const Json grid{{"schema", "atx.nav-construction-grid/v1"},
                  {"variants", Json::array({{{"id", "a"}, {"flags", {{"--cadence", "2"}}}}})}};
  write_json_file(verb.dir.path / "grid.json", grid);
  EXPECT_EQ(refused("grid", "1.5",
                    {"--calibrate-gross", text(g), "--construction-grid",
                     (verb.dir.path / "grid.json").string()}),
            2);
  // Another rule: aim_leverage is aim-partial-v5's key.
  std::vector<std::string> args{
      "nav", "--combined", verb.inputs.combined, "--combined-sha256", verb.inputs.combined_sha256,
      "--role", verb.inputs.role, "--role-sha256", verb.inputs.role_sha256, "--output",
      (verb.dir.path / "baseline").string(), "--rule", "baseline-v1", "--calibrate-gross",
      text(g)};
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  std::ostringstream out, err;
  EXPECT_EQ(st::dispatch_nav_replay(static_cast<int>(argv.size()), argv.data(), out, err), 1)
      << err.str();
  EXPECT_FALSE(std::filesystem::exists(verb.dir.path / "baseline"));
}

} // namespace
