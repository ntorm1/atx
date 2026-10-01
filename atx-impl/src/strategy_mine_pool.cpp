#include "strategy_mine_pool.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/sha256.hpp"
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

// load_rows' checks of one payload, streamed through one io_chunk buffer and kept nowhere (lane
// MINE-MEM): the extent, the read, the extent again and the SHA-256 in the order and words of
// ic_detail::load_pinned_f64, then load_rows' infinity check.
co::Status check_row(const MinePoolManifest &manifest, const MinePoolFile &row, usize cells) {
  const auto path = std::filesystem::path(manifest.path).parent_path() / row.file;
  const std::string label = "IC runner: mine pool payload " + row.name;
  const std::string name = path.filename().string();
  const u64 bytes = static_cast<u64>(cells) * sizeof(f64);
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in || in.tellg() < 0 || static_cast<u64>(in.tellg()) != bytes)
    return co::Err(co::ErrorCode::InvalidArgument, label + " extent: " + name);
  in.seekg(0);
  std::vector<f64> buffer(icd::io_chunk / sizeof(f64));
  co::Sha256 digest;
  bool infinite = false;
  for (u64 offset = 0; offset < bytes;) {
    const usize count = static_cast<usize>(
        std::min<u64>(static_cast<u64>(buffer.size()), (bytes - offset) / sizeof(f64)));
    const std::span<const f64> values = std::span<const f64>(buffer).first(count);
    const auto chunk = std::as_writable_bytes(std::span<f64>(buffer).first(count));
    // SAFETY: char accesses the object representation of trivially copyable f64 storage.
    in.read(reinterpret_cast<char *>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
    if (!in) return co::Err(co::ErrorCode::IoError, label + " truncated: " + name);
    ATX_TRY_VOID(digest.update(chunk));
    infinite = infinite ||
               std::any_of(values.begin(), values.end(), [](f64 v) { return std::isinf(v); });
    offset += static_cast<u64>(chunk.size());
  }
  if (in.peek() != std::char_traits<char>::eof())
    return co::Err(co::ErrorCode::IoError, label + " changed extent: " + name);
  ATX_TRY(const auto actual, digest.finalize());
  if (icd::hex(actual) != row.sha256)
    return co::Err(co::ErrorCode::InvalidArgument, label + " SHA256 mismatch: " + name);
  if (infinite)
    return co::Err(fail(co::ErrorCode::InvalidArgument, "infinite value in " + row.name));
  return co::Ok();
}

co::Status check_binding(const MinePoolManifest &manifest, const ResearchRole &role) {
  const auto &panel = role.data().panel;
  if (manifest.role_manifest_sha256 != role.data().manifest_sha256 ||
      manifest.dates != panel.dates() || manifest.instruments != panel.instruments())
    return co::Err(fail(co::ErrorCode::InvalidArgument,
                        "the pool is bound to another role (manifest SHA-256 or axes differ)"));
  return co::Ok();
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

co::Result<MinePool> bind_mine_pool(const MinePoolManifest &manifest, const ResearchRole &role) {
  MinePool out;
  if (manifest.path.empty()) return co::Ok(std::move(out));
  ATX_TRY_VOID(check_binding(manifest, role));
  out.path = manifest.path;
  out.sha256 = manifest.sha256;
  const usize cells = role.data().panel.cells();
  ATX_TRY(out.regressors, load_rows(manifest, manifest.regressors, cells));
  for (const MinePoolFile &row : manifest.members) ATX_TRY_VOID(check_row(manifest, row, cells));
  return co::Ok(std::move(out));
}

co::Status load_mine_pool_members(const MinePoolManifest &manifest, const ResearchRole &role,
                                  MinePool &pool) {
  if (manifest.path.empty()) return co::Ok();
  ATX_TRY_VOID(check_binding(manifest, role));
  ATX_TRY(pool.members, load_rows(manifest, manifest.members, role.data().panel.cells()));
  return co::Ok();
}

} // namespace atx::impl::strategy
