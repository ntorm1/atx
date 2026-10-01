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

// One pinned payload read front to back and kept nowhere (lane MINE-MEM; shared with the member
// stream by lane MINE-JOIN): open checks the extent; each read is hashed and scanned for an
// infinite value; close checks the extent again and the SHA-256, in the order and words of
// ic_detail::load_pinned_f64, then load_rows' infinity check.
struct PinnedPayload {
  std::string label; // "IC runner: mine pool payload NAME", the words of load_rows' refusals
  std::string file;  // the payload's file name
  std::string name;  // the pool row's name
  std::string sha256;
  std::ifstream in;
  co::Sha256 digest;
  bool infinite{};
};

co::Status open_payload(PinnedPayload &payload, const std::filesystem::path &dir,
                        const MinePoolFile &row, usize cells) {
  const auto path = dir / row.file;
  payload.label = "IC runner: mine pool payload " + row.name;
  payload.file = path.filename().string();
  payload.name = row.name;
  payload.sha256 = row.sha256;
  const u64 bytes = static_cast<u64>(cells) * sizeof(f64);
  payload.in.open(path, std::ios::binary | std::ios::ate);
  if (!payload.in || payload.in.tellg() < 0 || static_cast<u64>(payload.in.tellg()) != bytes)
    return co::Err(co::ErrorCode::InvalidArgument, payload.label + " extent: " + payload.file);
  payload.in.seekg(0);
  return co::Ok();
}

// Reads the payload's next values.size() values into `values`, as stored.
co::Status read_payload(PinnedPayload &payload, std::span<f64> values) {
  const std::span<std::byte> bytes = std::as_writable_bytes(values);
  // SAFETY: char accesses the object representation of trivially copyable f64 storage.
  payload.in.read(reinterpret_cast<char *>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
  if (!payload.in)
    return co::Err(co::ErrorCode::IoError, payload.label + " truncated: " + payload.file);
  ATX_TRY_VOID(payload.digest.update(bytes));
  payload.infinite = payload.infinite || std::any_of(values.begin(), values.end(),
                                                     [](f64 v) { return std::isinf(v); });
  return co::Ok();
}

// After the last read: the payload ended where its extent said, hashes to its pin, and holds no
// infinite value.
co::Status close_payload(PinnedPayload &payload) {
  if (payload.in.peek() != std::char_traits<char>::eof())
    return co::Err(co::ErrorCode::IoError, payload.label + " changed extent: " + payload.file);
  ATX_TRY(const auto actual, payload.digest.finalize());
  if (icd::hex(actual) != payload.sha256)
    return co::Err(co::ErrorCode::InvalidArgument,
                   payload.label + " SHA256 mismatch: " + payload.file);
  if (payload.infinite)
    return co::Err(fail(co::ErrorCode::InvalidArgument, "infinite value in " + payload.name));
  return co::Ok();
}

// One payload through one io_chunk buffer, kept nowhere.
co::Status check_row(const std::filesystem::path &dir, const MinePoolFile &row, usize cells) {
  PinnedPayload payload;
  ATX_TRY_VOID(open_payload(payload, dir, row, cells));
  std::vector<f64> buffer(icd::io_chunk / sizeof(f64));
  for (usize offset = 0; offset < cells;) {
    const usize count = std::min(buffer.size(), cells - offset);
    ATX_TRY_VOID(read_payload(payload, std::span<f64>(buffer).first(count)));
    offset += count;
  }
  return close_payload(payload);
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
  out.dates = manifest.dates; // == the role's axes (check_binding)
  out.instruments = manifest.instruments;
  ATX_TRY(out.regressors, load_rows(manifest, manifest.regressors, role.data().panel.cells()));
  out.members = manifest.members;
  ATX_TRY_VOID(check_mine_pool_members(out));
  return co::Ok(std::move(out));
}

co::Status check_mine_pool_members(const MinePool &pool) {
  const auto dir = std::filesystem::path(pool.path).parent_path();
  const usize cells = pool.dates * pool.instruments;
  for (const MinePoolFile &row : pool.members) ATX_TRY_VOID(check_row(dir, row, cells));
  return co::Ok();
}

co::Status stream_mine_pool_members(const MinePool &pool, usize begin, usize end,
                                    const MinePoolRowsFn &on_rows) {
  if (begin > end || end > pool.dates)
    return co::Err(fail(co::ErrorCode::InvalidArgument,
                        "member rows [" + std::to_string(begin) + ", " + std::to_string(end) +
                            ") outside the pool's " + std::to_string(pool.dates) + " dates"));
  const auto dir = std::filesystem::path(pool.path).parent_path();
  const usize names = pool.instruments;
  const usize members = pool.members.size();
  // Every payload is read whole, in lockstep, so each is hashed and checked as check_row checks
  // it; only one date of each is held.
  std::vector<PinnedPayload> payloads(members);
  for (usize k = 0; k < members; ++k)
    ATX_TRY_VOID(open_payload(payloads[k], dir, pool.members[k], pool.dates * names));
  std::vector<std::vector<f64>> rows(members, std::vector<f64>(names));
  std::vector<std::span<const f64>> views;
  views.reserve(members);
  for (const std::vector<f64> &row : rows) views.emplace_back(row);
  for (usize date = 0; date < pool.dates; ++date) {
    for (usize k = 0; k < members; ++k) ATX_TRY_VOID(read_payload(payloads[k], rows[k]));
    if (date >= begin && date < end) ATX_TRY_VOID(on_rows(date, views));
  }
  for (PinnedPayload &payload : payloads) ATX_TRY_VOID(close_payload(payload));
  return co::Ok();
}

} // namespace atx::impl::strategy
