#pragma once

#include "atx/server/research.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

#include <duckdb.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"

namespace {
using Json = nlohmann::json;

std::string digest(std::string_view bytes) {
  const auto value = atx::core::sha256_hex(bytes);
  if (!value) throw std::runtime_error(value.error().message());
  return *value;
}

std::string file_bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("fixture file missing");
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

// C API handles are owned only here; every failure and destruction closes them.
class FixtureConnection {
public:
  FixtureConnection() {
    if (duckdb_open(nullptr, &database_) != DuckDBSuccess) {
      duckdb_close(&database_);
      throw std::runtime_error("fixture database open failed");
    }
    if (duckdb_connect(database_, &connection_) != DuckDBSuccess) {
      duckdb_close(&database_);
      throw std::runtime_error("fixture database connect failed");
    }
  }
  ~FixtureConnection() {
    duckdb_disconnect(&connection_);
    duckdb_close(&database_);
  }
  FixtureConnection(const FixtureConnection&) = delete;
  FixtureConnection& operator=(const FixtureConnection&) = delete;
  FixtureConnection(FixtureConnection&&) = delete;
  FixtureConnection& operator=(FixtureConnection&&) = delete;

  void query(const std::string& sql) const {
    duckdb_result result{};
    const auto state = duckdb_query(connection_, sql.c_str(), &result);
    const auto* error = duckdb_result_error(&result);
    const std::string message = error ? error : "fixture SQL failed";
    duckdb_destroy_result(&result);
    if (state != DuckDBSuccess) throw std::runtime_error(message);
  }
private:
  duckdb_database database_{};
  duckdb_connection connection_{};
};

class DatasetResearchTest : public testing::Test {
protected:
  std::filesystem::path root;
  Json manifest;
  atx::server::ResearchOptions options;

  void SetUp() override {
    auto unique = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::string letters;
    for (std::size_t i = 0; i < 14; ++i) {
      letters += static_cast<char>('a' + unique % 26);
      unique /= 26;
    }
    root = std::filesystem::temp_directory_path() / ("atx-dataset-test-" + letters);
    std::filesystem::create_directory(root);
    std::filesystem::create_directory(root / "year=2019");
    std::filesystem::create_directory(root / "year=2020");
    FixtureConnection connection;
    connection.query("SET threads=1");
    connection.query("SET memory_limit='128MB'");
    connection.query("SET temp_directory=''");
    connection.query("CREATE TABLE observations AS WITH dates AS (SELECT d::DATE AS session_date, "
        "row_number() OVER (ORDER BY d)-1 AS t FROM generate_series(DATE '2019-12-02', "
        "DATE '2020-02-14', INTERVAL '1 DAY') q(d) WHERE isodow(d)<6) "
        "SELECT session_date, id::BIGINT AS security_id, "
        "100*pow(1+(id-2.5)*0.002,t) AS raw_close, "
        "999*pow(1+(id-2.5)*0.002,t) AS close, 1000000.0::DOUBLE AS volume, "
        "((id-2.5)*0.002)::DOUBLE AS ret, false AS ret_guarded, "
        "true AS member, true AS member_equity, (50000000.0+id)::DOUBLE AS adv63 "
        "FROM dates CROSS JOIN range(1,5) n(id)");
    manifest = {{"schema", "atx.alpha-panel.panel/v2"}, {"status", "complete"},
        {"rules", "isolated test fixture; prior-session membership declared for integration"},
        {"code", {{"git_head", "fixture"}}}, {"files", Json::object()},
        {"input_manifests_sha256", Json::object()}};
    const std::array<std::pair<int, int>, 3> months{{{2019, 12}, {2020, 1}, {2020, 2}}};
    for (const auto& entry : months) {
      const auto [year, month] = entry;
      const auto name = "year=" + std::to_string(year) + "/panel-" +
          (month < 10 ? "0" : "") + std::to_string(month) + ".parquet";
      const auto path = (root / name).generic_string();
      connection.query("COPY (SELECT * FROM observations WHERE year(session_date)=" +
          std::to_string(year) + " AND month(session_date)=" + std::to_string(month) +
          ") TO '" + path + "' (FORMAT PARQUET)");
      const auto bytes = file_bytes(root / name);
      manifest["files"][name] = {{"bytes", bytes.size()}, {"sha256", digest(bytes)}};
    }
    options.repo_root = root;
    options.artifact_roots = {root};
    pin_manifest();
  }

  void TearDown() override {
    const auto parent = std::filesystem::canonical(std::filesystem::temp_directory_path());
    const auto target = std::filesystem::canonical(root);
    ASSERT_EQ(target.parent_path(), parent);
    ASSERT_TRUE(target.filename().string().starts_with("atx-dataset-test-"));
    std::error_code error;
    std::filesystem::remove_all(target, error);
    if (error) std::cerr << "fixture cleanup: " << error.message() << '\n';
  }

  void pin_manifest() {
    const auto bytes = manifest.dump();
    std::ofstream output(root / "manifest.json", std::ios::binary);
    output << bytes;
    output.close();
    options.datasets = {{"prices-train", "TRAIN integration fixture", root / "manifest.json",
                         digest(bytes)}};
  }

  Json request() const {
    return {{"mode", "real_dsl"}, {"dataset_id", "prices-train"},
        {"dataset_sha256", options.datasets.front().manifest_sha256},
        {"strategy_dsl", "returns"}, {"start_date", "2020-01-20"},
        {"end_date", "2020-02-14"}, {"instrument_limit", 4},
        {"initial_capital", 1e6}, {"trade_bps", 0}, {"annual_borrow_bps", 0}};
  }

  Json execute(const Json& input) const {
    return Json::parse(atx::server::ResearchBackend{options}.execute(input.dump(), {}, {}));
  }

  void rewrite_month(std::string_view name, const std::string& update) {
    const auto original = root / name;
    const auto replacement = original.parent_path() / "replacement.parquet";
    {
      FixtureConnection connection;
      connection.query("CREATE TABLE revision AS SELECT * FROM read_parquet('" +
          original.generic_string() + "', hive_partitioning=false)");
      connection.query(update);
      connection.query("COPY revision TO '" + replacement.generic_string() +
          "' (FORMAT PARQUET)");
    }
    // Only this test's verified temporary directory is mutated.
    std::filesystem::remove(original);
    std::filesystem::rename(replacement, original);
    const auto bytes = file_bytes(original);
    manifest["files"][std::string(name)] = {{"bytes", bytes.size()}, {"sha256", digest(bytes)}};
    pin_manifest();
  }
};

} // namespace
