#include "strategy_mine_pool.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "atx/engine/combine/marginal_rank_ic.hpp"
#include "strategy_ic_detail.hpp"

namespace atx::impl::strategy {
namespace {
namespace co = atx::core;
namespace icd = ic_detail;
using Json = nlohmann::json;

constexpr const char *kPoolSchema = "atx.mine-pool/v1";

co::Error fail(co::ErrorCode code, const std::string &message) {
  return co::Error{code, "mine pool: " + message};
}

std::string text_of(const Json &j, const char *key) {
  return j.is_object() && j.contains(key) && j.at(key).is_string() ? j.at(key).get<std::string>()
                                                                   : std::string{};
}

// One list of rows: names are DSL-style identifiers, unique within the list; each payload is
// NAME.f64 beside the manifest with the full panel extent.
co::Result<std::vector<MinePoolFile>> pool_rows(const Json &j, const char *key, usize cap,
                                                u64 bytes) {
  std::vector<MinePoolFile> out;
  if (!j.contains(key)) return co::Ok(std::move(out));
  const Json &rows = j.at(key);
  if (!rows.is_array() || rows.size() > cap)
    return co::Err(fail(co::ErrorCode::InvalidArgument,
                        std::string(key) + ": an array of at most " + std::to_string(cap) +
                            " rows"));
  std::set<std::string> names;
  for (const Json &row : rows) {
    MinePoolFile file{text_of(row, "name"), text_of(row, "file"), text_of(row, "sha256")};
    const bool extent = row.is_object() && row.contains("bytes") &&
                        row.at("bytes").is_number_unsigned() && row.at("bytes").get<u64>() == bytes;
    if (!icd::field_identifier(file.name) || file.file != file.name + ".f64" ||
        !icd::hash_valid(file.sha256) || !extent || !names.insert(file.name).second)
      return co::Err(fail(co::ErrorCode::InvalidArgument, std::string(key) + " row: " + file.name));
    out.push_back(std::move(file));
  }
  return co::Ok(std::move(out));
}

co::Result<std::vector<MinePoolColumn>> load_rows(const MinePoolManifest &manifest,
                                                  const std::vector<MinePoolFile> &rows,
                                                  usize cells) {
  const auto dir = std::filesystem::path(manifest.path).parent_path();
  std::vector<MinePoolColumn> out;
  for (const MinePoolFile &row : rows) {
    MinePoolColumn column{row.name, row.sha256, {}};
    ATX_TRY_VOID(icd::load_pinned_f64(dir / row.file, row.sha256, cells, column.values,
                                      "mine pool payload " + row.name, nullptr, true));
    if (std::any_of(column.values.begin(), column.values.end(),
                    [](f64 v) { return std::isinf(v); }))
      return co::Err(fail(co::ErrorCode::InvalidArgument, "infinite value in " + row.name));
    out.push_back(std::move(column));
  }
  return co::Ok(std::move(out));
}
} // namespace

co::Result<MinePoolManifest> read_mine_pool_manifest(const std::string &path,
                                                     const std::string &pin) {
  MinePoolManifest out;
  if (path.empty()) return co::Ok(std::move(out));
  try {
    ATX_TRY(const auto j, icd::pinned_json(path, pin));
    const bool axes = j.is_object() && j.contains("dates") && j.at("dates").is_number_unsigned() &&
                      j.contains("instruments") && j.at("instruments").is_number_unsigned();
    if (!axes || text_of(j, "schema") != kPoolSchema || text_of(j, "status") != "complete" ||
        !icd::hash_valid(text_of(j, "role_manifest_sha256")))
      return co::Err(fail(co::ErrorCode::InvalidArgument,
                          "not a complete atx.mine-pool/v1 manifest: " + path));
    out.path = path;
    out.sha256 = pin;
    out.role_manifest_sha256 = text_of(j, "role_manifest_sha256");
    const u64 dates = j.at("dates").get<u64>();
    const u64 names = j.at("instruments").get<u64>();
    if (dates == 0U || dates > 4096U || names == 0U || names > 20000U)
      return co::Err(fail(co::ErrorCode::InvalidArgument, "pool axes: " + path));
    out.dates = static_cast<usize>(dates);
    out.instruments = static_cast<usize>(names);
    const u64 bytes = dates * names * sizeof(f64);
    ATX_TRY(out.regressors, pool_rows(j, "regressors",
                                      atx::engine::combine::kMaxMarginalRegressors, bytes));
    ATX_TRY(out.members, pool_rows(j, "members", kMaxMinePoolMembers, bytes));
    return co::Ok(std::move(out));
  } catch (const std::exception &e) {
    return co::Err(fail(co::ErrorCode::InvalidArgument, e.what()));
  }
}

co::Result<MinePool> load_mine_pool(const MinePoolManifest &manifest, const ResearchRole &role) {
  MinePool out;
  if (manifest.path.empty()) return co::Ok(std::move(out));
  const auto &panel = role.data().panel;
  if (manifest.role_manifest_sha256 != role.data().manifest_sha256 ||
      manifest.dates != panel.dates() || manifest.instruments != panel.instruments())
    return co::Err(fail(co::ErrorCode::InvalidArgument,
                        "the pool is bound to another role (manifest SHA-256 or axes differ)"));
  out.path = manifest.path;
  out.sha256 = manifest.sha256;
  ATX_TRY(out.regressors, load_rows(manifest, manifest.regressors, panel.cells()));
  ATX_TRY(out.members, load_rows(manifest, manifest.members, panel.cells()));
  return co::Ok(std::move(out));
}

} // namespace atx::impl::strategy
