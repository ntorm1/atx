// NAV replay: pinned loaders (fields, role geometry, admission).
// P9 C3 (task 1, move-only): moved unchanged from strategy_nav_replay.cpp.

#include "book/nav_replay_internal.hpp"

namespace atx::impl::strategy {
using namespace nav_book;
namespace {
bool hash_valid(std::string_view s) {
  return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
// The whole file, when its SHA-256 equals the external pin and it fits `cap`.
co::Result<Json> pinned_document(const std::string& path, const std::string& pin, u64 cap,
                                 const char* what) {
  const std::string label = std::string("nav replay: ") + what;
  if (!hash_valid(pin)) return co::Err(co::ErrorCode::InvalidArgument, label + " pin");
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file || file.tellg() <= 0 || static_cast<u64>(file.tellg()) > cap)
    return co::Err(co::ErrorCode::InvalidArgument, label + " missing/oversized");
  std::string text(static_cast<usize>(file.tellg()), '\0'); file.seekg(0);
  file.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (!file || file.peek() != std::char_traits<char>::eof())
    return co::Err(co::ErrorCode::IoError, label + " changed extent");
  ATX_TRY(auto digest, co::sha256_hex(text));
  if (digest != pin) return co::Err(co::ErrorCode::InvalidArgument, label + " SHA");
  return co::Ok(Json::parse(text));
}
// Exactly `cells` little-endian f64 whose bytes hash to `sha`.
co::Result<std::vector<f64>> read_field(const std::filesystem::path& path, u64 cells,
                                        const std::string& sha) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file || file.tellg() < 0 || static_cast<u64>(file.tellg()) != cells * sizeof(f64))
    return co::Err(co::ErrorCode::IoError, "nav replay: field payload extent");
  file.seekg(0);
  std::vector<f64> out(static_cast<usize>(cells));
  const auto bytes = std::as_writable_bytes(std::span(out));
  // SAFETY: char accesses the object representation of a trivially copyable f64 array.
  file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!file || file.peek() != std::char_traits<char>::eof())
    return co::Err(co::ErrorCode::IoError, "nav replay: truncated/extended field payload");
  ATX_TRY(auto digest, co::sha256_hex(std::span<const std::byte>(bytes)));
  if (digest != sha)
    return co::Err(co::ErrorCode::InvalidArgument, "nav replay: field payload SHA");
  return co::Ok(std::move(out));
}
// The manifest entry of a field the replay uses: exactly one, declared
// point_in_time true (a missing key refuses), <f8 date-major role geometry, file
// "<name>.f64" (never a path), and a receipt agreeing with the entry SHA.
co::Result<Json> used_field(const Json& manifest, const std::string& name, u64 dates, u64 names) {
  const Json* found = nullptr;
  for (const auto& entry : manifest.at("fields")) {
    if (entry.at("name").get<std::string>() != name) continue;
    if (found)
      return co::Err(co::ErrorCode::InvalidArgument, "nav replay: duplicate field " + name);
    found = &entry;
  }
  if (!found) return co::Err(co::ErrorCode::InvalidArgument, "nav replay: fields lack " + name);
  const auto& e = *found;
  if (!e.contains("point_in_time") || !e.at("point_in_time").is_boolean() ||
      !e.at("point_in_time").get<bool>())
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: field not declared point in time: " + name);
  const auto sha = e.at("sha256").get<std::string>();
  const auto& receipt = manifest.at("files").at(name + ".f64");
  if (e.at("file").get<std::string>() != name + ".f64" || e.at("dtype") != "<f8" ||
      e.at("layout") != "date-major" || e.at("shape") != Json::array({dates, names}) ||
      !hash_valid(sha) || receipt.at("sha256").get<std::string>() != sha ||
      receipt.at("bytes").get<u64>() != dates * names * sizeof(f64))
    return co::Err(co::ErrorCode::InvalidArgument, "nav replay: field layout/receipt: " + name);
  return co::Ok(e);
}
// P9 C3 (Ruling P13, the NAV half of the C++ seal consumer; S2's IC half is
// strategy_ic_admission.cpp fields_seal): "match" when the fields manifest's seal.exclusive_end is
// this build's research seal, "absent" for a legacy manifest without one (no seal block, or a
// block without exclusive_end), accepted and logged by the caller. A present exclusive_end that is
// not this build's seal, or a malformed seal block, refuses before any payload is read.
co::Result<std::string> fields_seal(const Json& manifest) {
  const auto seal = manifest.find("seal");
  if (seal == manifest.end()) return co::Ok(std::string("absent"));
  if (!seal->is_object())
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: fields manifest seal block is not an object");
  const auto recorded = seal->find("exclusive_end");
  if (recorded == seal->end()) return co::Ok(std::string("absent"));
  if (!recorded->is_string() || recorded->get<std::string>() != engine::data::kSealBeginDate)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: fields manifest was built under the seal " + recorded->dump() +
                       "; this build's seal is " + std::string(engine::data::kSealBeginDate) +
                       " (" + std::string(engine::data::kResearchWindowId) + ")");
  return co::Ok(std::string("match"));
}
} // namespace
namespace nav_book {
// Console only (the run's published bytes do not record it, so a legacy and a sealed manifest
// with the same fields publish the same files).
void log_fields_seal(const LoadedFields& fields, std::ostream& progress) {
  if (fields.seal == "absent")
    progress << "nav replay: fields-seal seal=absent accepted-as-legacy-manifest\n";
}
} // namespace nav_book
namespace {
// One used field (used_field checks, payload SHA) and its recipe record in `used`.
co::Result<std::vector<f64>> load_field(const Json& manifest, const std::filesystem::path& base,
                                        const std::string& name, u64 dates, u64 names,
                                        Json& used) {
  ATX_TRY(const auto entry, used_field(manifest, name, dates, names));
  const auto sha = entry.at("sha256").get<std::string>();
  ATX_TRY(auto values, read_field(base / (name + ".f64"), dates * names, sha));
  used[name] = Json{{"sha256", sha}, {"point_in_time", true},
      {"clock", entry.contains("clock") ? entry.at("clock") : Json(nullptr)},
      {"staleness", entry.contains("staleness") ? entry.at("staleness") : Json(nullptr)}};
  return co::Ok(std::move(values));
}
// What load_fields loads besides the borrow fields (P9 AL-CLOCK): the info clock for the rule
// (recorded in fields_used, an input of the run) or for the print alone (not recorded).
enum class ClockLoad : u8 { None = 0, Rule = 1, PrintOnly = 2 };
// Pins the atx.research-role-fields/v1 manifest, then its role to the pinned --role
// (manifest, sessions, ids, member SHA against the role manifest's own receipts),
// then loads shares_out and si_shares (and, for the industry ids, grp_ff12; P9 AL-CLOCK: with
// `clock`, ea_days_since) within `budget` bytes. Nothing is written.
co::Result<LoadedFields> load_fields(const NavFieldsPin& pin, const TargetReplayRunConfig& cfg,
                                     u64 budget, ClockLoad clock) {
  ATX_TRY(auto m, pinned_document(pin.manifest_path, pin.manifest_sha256,
                                  max_fields_manifest_bytes, "fields manifest"));
  if (m.at("schema") != fields_schema || m.at("status") != "complete")
    return co::Err(co::ErrorCode::InvalidArgument, "nav replay: fields manifest schema/status");
  ATX_TRY(auto role, pinned_document(cfg.role_path, cfg.role_sha256, max_role_manifest_bytes,
                                     "role manifest"));
  const auto& pins = m.at("role"); const auto& receipts = role.at("files");
  if (pins.at("manifest_sha256").get<std::string>() != cfg.role_sha256 ||
      pins.at("sessions_sha256") != receipts.at("sessions.i64").at("sha256") ||
      pins.at("ids_sha256") != receipts.at("ids.u64").at("sha256") ||
      pins.at("member_sha256") != receipts.at("member.u8").at("sha256"))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: fields manifest role pins do not match --role");
  const auto dates = role.at("dates").get<u64>(), names = role.at("instruments").get<u64>();
  if (!dates || dates > max_dates || !names || names > max_names)
    return co::Err(co::ErrorCode::InvalidArgument, "nav replay: fields role geometry");
  const u64 cells = dates * names;
  const bool industry = neutralize_by_industry(cfg.target.neutralize);
  const u64 count = financing_field_names.size() + (industry ? 1U : 0U) +
                    (clock != ClockLoad::None ? 1U : 0U);
  if (cells > budget / (count * sizeof(f64)))
    return co::Err(co::ErrorCode::OutOfRange, "nav replay: fields workspace budget");
  LoadedFields out;
  ATX_TRY(out.seal, fields_seal(m));
  Json used = Json::object();
  const auto base = std::filesystem::path(pin.manifest_path).parent_path();
  for (const char* field : financing_field_names) {
    const std::string name = field;
    ATX_TRY(auto values, load_field(m, base, name, dates, names, used));
    (name == "shares_out" ? out.shares_out : out.si_shares) = std::move(values);
  }
  if (industry) {
    ATX_TRY(out.industry, load_field(m, base, industry_group_field, dates, names, used));
  }
  if (clock != ClockLoad::None) { // P9 AL-CLOCK: the print alone leaves fields_used unchanged
    Json printed = Json::object();
    const std::string name = nav_info_clock_field;
    ATX_TRY(out.info_clock, load_field(m, base, name, dates, names,
                                       clock == ClockLoad::Rule ? used : printed));
    const auto& record = clock == ClockLoad::Rule ? used : printed;
    out.info_clock_sha256 = record.at(name).at("sha256").get<std::string>();
  }
  out.binding = Json{{"schema", fields_schema},
      {"manifest_sha256", pin.manifest_sha256}, {"role_manifest_sha256", cfg.role_sha256},
      {"fields_used", std::move(used)},
      {"fields_not_used", "mktcap_lagged (presence not point in time): market cap = "
                          "shares_out x raw_close only"}};
  return co::Ok(std::move(out));
}
// The run geometry, from the pinned role manifest alone (no payload is read): the
// loader later requires the blend's dates, instruments and score window to equal it.
struct RoleGeometry {
  usize names{}, sessions{}; // instruments; score_end - score_begin
  usize score_begin{};       // the pre-score history a warm start may use
  usize dates{};             // the role's rows (a label role's marks are charged per cell)
};
co::Result<RoleGeometry> role_geometry(const TargetReplayRunConfig& cfg) {
  ATX_TRY(auto role, pinned_document(cfg.role_path, cfg.role_sha256, max_role_manifest_bytes,
                                     "role manifest"));
  const auto dates = role.at("dates").get<u64>(), names = role.at("instruments").get<u64>();
  const auto begin = role.at("score_begin").get<u64>(), end = role.at("score_end").get<u64>();
  if (!dates || dates > max_dates || !names || names > max_names || begin >= end || end > dates)
    return co::Err(co::ErrorCode::InvalidArgument, "nav replay: role geometry");
  return co::Ok(RoleGeometry{static_cast<usize>(names), static_cast<usize>(end - begin),
                             static_cast<usize>(begin), static_cast<usize>(dates)});
}
} // namespace
namespace nav_book {
// Admission and load of one pinned run, shared by run_nav_replay and the decide path:
// the workspace reserve of `books` books at the pinned role's own geometry (manifest
// only), then the fields (pinned and role-checked) and the blend charged against what
// remains, so input + all results fit one budget. `label` (v8 E-25, --label-role): its
// manifest is checked against --role's before any payload is read, its marks are charged
// before the fields, and they load (checked against the blend) last.
co::Result<AdmittedRun> admit_and_load(const TargetReplayRunConfig& cfg,
                                       const NavReplayConfig& base, const NavFieldsPin& fields,
                                       usize books, bool holdings,
                                       const NavLabelRolePin& label, bool info_clock_print) {
  const bool tiered = !fields.manifest_path.empty();
  const bool labelled = !label.manifest_path.empty();
  // P9 AL-CLOCK: the rule's clock is an input of the run (fields_used); the print's is not.
  const ClockLoad clock = base.rate == NavRateRule::InfoClockV1 ? ClockLoad::Rule
                          : info_clock_print                    ? ClockLoad::PrintOnly
                                                                : ClockLoad::None;
  if (clock != ClockLoad::None && !tiered)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: --rate info-clock-rate-v1 and --info-clock-print need --fields "
                   "(the role fields manifest holding ea_days_since)");
  ATX_TRY(const auto geometry, role_geometry(cfg));
  // A warm start longer than the pinned role's pre-score history is refused before any
  // payload is read (the replay's own input check repeats it).
  ATX_TRY_VOID(check_warm_start(base.warm_start_sessions, geometry.score_begin));
  // v8 E-25: the label role against --role, from the two pinned manifests alone.
  if (labelled)
    ATX_TRY_VOID(detail::check_label_role(cfg, label.manifest_path, label.manifest_sha256));
  const u64 reserve = nav_workspace_reserve_bytes(base, books, tiered, geometry.names,
                                                  geometry.sessions, holdings);
  if (cfg.target.max_working_bytes <= reserve)
    return co::Err(co::ErrorCode::OutOfRange, "nav replay: budget below NAV workspace reserve");
  u64 remaining = cfg.target.max_working_bytes - reserve;
  if (labelled) {
    const u64 marks = u64{geometry.dates} * geometry.names * detail::label_role_cell_bytes +
                      detail::label_role_metadata_bytes;
    if (remaining <= marks)
      return co::Err(co::ErrorCode::OutOfRange,
                     "nav replay: budget below the label role's marks");
    remaining -= marks;
  }
  AdmittedRun out;
  if (tiered) {
    // Pinned and role-checked before the blend is loaded or anything is written.
    ATX_TRY(out.fields, load_fields(fields, cfg, remaining, clock));
    remaining -= (out.fields.shares_out.size() + out.fields.si_shares.size() +
                  out.fields.industry.size() + out.fields.info_clock.size()) * sizeof(f64);
  }
  auto load = cfg; load.target.max_working_bytes = remaining;
  ATX_TRY(out.blend, detail::load_saved_blend(load, true));
  // The reserve's geometry is the loaded one (same pinned role manifest).
  if (out.blend.names != geometry.names || out.blend.end - out.blend.begin != geometry.sessions ||
      (tiered && out.fields.shares_out.size() != out.blend.signal.size()))
    return co::Err(co::ErrorCode::InvalidArgument, "nav replay: fields/blend geometry");
  if (labelled) {
    ATX_TRY(out.label, detail::load_label_role(cfg, label.manifest_path, label.manifest_sha256,
                                               out.blend));
  }
  return co::Ok(std::move(out));
}
} // namespace nav_book
} // namespace atx::impl::strategy
