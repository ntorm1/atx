#include "strategy_ic_absent.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include "atx/core/sha256.hpp"
#include "strategy_ic_detail.hpp"
#include "strategy_ic_runner.hpp"

namespace atx::impl::strategy {
namespace {
namespace co = atx::core;
using Json = nlohmann::json;

[[nodiscard]] auto refuse(const std::string& why) {
  return co::Err(co::ErrorCode::InvalidArgument, "IC runner: --absent-members " + why);
}
// The member id spelling of the drop list (contract section 2): [A-Za-z0-9_]+.
[[nodiscard]] bool member_id(const std::string& id) noexcept {
  return !id.empty() && std::all_of(id.begin(), id.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
  });
}
// The pinned file's members against the library (both already admitted): ids as listed, and per
// candidate 1 when listed. A list longer than the library refuses at its first id that is not a
// candidate or is repeated, so the scan is bounded by the library's candidate count.
co::Result<IcAbsentMembers> checked_list(const Json& doc, const ic_detail::Library& lib,
                                         const std::string& sha256) {
  if (!doc.is_object()) return refuse("file is not a JSON object");
  const auto schema = doc.find("schema");
  if (schema == doc.end() || !schema->is_string() ||
      schema->get_ref<const std::string&>() != absent_members_schema)
    return refuse("schema must be " + std::string(absent_members_schema));
  const auto members = doc.find("members");
  if (members == doc.end() || !members->is_array() || members->empty())
    return refuse("members must be a non-empty array of candidate ids");
  IcAbsentMembers list;
  list.list_sha256 = sha256;
  list.absent.assign(lib.candidates.size(), 0);
  list.members.reserve(members->size());
  for (const auto& entry : *members) {
    if (!entry.is_string()) return refuse("member must be a string");
    const auto& id = entry.get_ref<const std::string&>();
    if (!member_id(id)) return refuse("member id must match [A-Za-z0-9_]+: " + id);
    const auto at = std::find_if(lib.candidates.begin(), lib.candidates.end(),
                                 [&](const ic_detail::Candidate& c) { return c.id == id; });
    if (at == lib.candidates.end()) return refuse("member is not a candidate of --library: " + id);
    auto& listed = list.absent[static_cast<std::size_t>(at - lib.candidates.begin())];
    if (listed != 0) return refuse("member listed twice: " + id);
    listed = 1;
    list.members.push_back(id);
  }
  return co::Ok(std::move(list));
}
} // namespace

co::Result<std::optional<IcAbsentMembers>> absent_members(const IcRunnerConfig& cfg,
                                                          const ic_detail::Library& lib) {
  const bool path = !cfg.absent_members_path.empty(), pin = !cfg.absent_members_sha256.empty();
  if (path != pin)
    return refuse("and --absent-members-sha256 are given together or not at all");
  if (!path) return co::Ok(std::optional<IcAbsentMembers>{});
  if (cfg.no_composition)
    return refuse("is refused with --no-composition (a screen builds no blend to remove it from)");
  if (!ic_detail::hash_valid(cfg.absent_members_sha256))
    return refuse("needs --absent-members-sha256 as 64 lowercase hex");
  ATX_TRY(const auto text, ic_detail::metadata_text(cfg.absent_members_path));
  ATX_TRY(const auto actual, co::sha256_hex(text));
  if (actual != cfg.absent_members_sha256)
    return refuse("file SHA-256 differs from --absent-members-sha256");
  const auto doc = Json::parse(text, nullptr, false);
  if (doc.is_discarded()) return refuse("file is not a JSON object");
  ATX_TRY(auto list, checked_list(doc, lib, cfg.absent_members_sha256));
  return co::Ok(std::optional<IcAbsentMembers>(std::move(list)));
}

Json absent_members_record(const IcAbsentMembers& list) {
  return Json{{"rule", std::string(absent_members_rule)},
              {"list_sha256", list.list_sha256},
              {"members", list.members}};
}
} // namespace atx::impl::strategy
