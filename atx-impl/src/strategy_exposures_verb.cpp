#include "strategy_exposures_verb.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <new>
#include <ostream>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "atx/engine/data/research_window.hpp"
#include "atx/engine/data/strategy_data.hpp"

namespace atx::impl::strategy {
namespace {
using namespace atx;
namespace co = atx::core;
namespace ed = atx::engine::data;
using Json = nlohmann::json;
constexpr u64 max_manifest_bytes = 16ULL << 20;
constexpr u64 max_dates = 4096, max_names = 20000;
constexpr const char* export_schema = "atx.price-risk-exposures/v1";
// What the files hold, in the fitter's vocabulary (fit_composition_weights.py
// CONTEXT_SEMANTICS), with the one difference that forward returns keep NaN.
constexpr const char* export_semantics =
    "price-risk-v1;beta252-min126-all-instrument-market;vol63-min32;ladv63;"
    "used=member&present&ok;z-sample-sd-clip5;min50;pivot1e-8;"
    "qr-basis-householder-lapack-sign;fwd=r[d+2]-valid-else-NaN;v1";
constexpr const char* fitter_context_semantics =
    "price-risk-v1;beta252-min126-all-instrument-market;vol63-min32;ladv63;"
    "used=member&present&ok;z-sample-sd-clip5;min50;pivot1e-8;qr-basis;"
    "fwd=r[d+2]-valid-else-0;v1";
constexpr const char* basis_layout =
    "decision-major, then name (role instrument order), then [q_intercept, q_beta, q_vol, "
    "q_log_adv]: row i of Q (numpy.linalg.qr of [1, z] over the used names ascending) on a used "
    "name, 0 elsewhere and on refused decisions; used iff q_intercept != 0";
constexpr const char* forward_layout =
    "decision-major, then name: the guarded adjusted simple return of interval d + 2 = "
    "(d+1, d+2], NaN when invalid (the fitter reads NaN as 0)";

bool hash_valid(std::string_view s) {
  return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
// The role manifest, verified against its pin before anything else is read.
co::Result<Json> pinned_manifest(const std::string& path, const std::string& pin) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file || file.tellg() <= 0 || static_cast<u64>(file.tellg()) > max_manifest_bytes)
    return co::Err(co::ErrorCode::InvalidArgument, "exposures: role manifest missing/oversized");
  std::string text(static_cast<usize>(file.tellg()), '\0');
  file.seekg(0);
  file.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (!file) return co::Err(co::ErrorCode::IoError, "exposures: role manifest read");
  ATX_TRY(auto digest, co::sha256_hex(text));
  if (digest != pin) return co::Err(co::ErrorCode::InvalidArgument, "exposures: role SHA-256");
  return co::Ok(Json::parse(text));
}
void write_f64(std::ofstream& file, std::span<const f64> values) {
  const auto bytes = std::as_bytes(values);
  // SAFETY: char writes the object representation of contiguous f64 data (the role
  // reader admits little-endian hosts only, so the files are little-endian).
  file.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
}
co::Status sealed(std::string_view what) {
  return co::Err(co::ErrorCode::InvalidArgument,
                 std::string("exposures: ") + std::string(what) + " reaches the " +
                     std::string(ed::kResearchWindowId) + " seal (2024-01-01)");
}
Json file_entry(const std::string& sha, u64 bytes, Json shape, const char* layout) {
  return Json{{"bytes", bytes}, {"sha256", sha}, {"dtype", "<f8"}, {"shape", std::move(shape)},
              {"layout", layout}};
}
} // namespace

co::Result<ExposureDecision> export_decision(const PriceExposureInput& panel,
                                             std::span<const u8> member,
                                             const PriceExposureConfig& cfg, usize d,
                                             ExposureExportScratch& s, std::span<f64> basis,
                                             std::span<f64> forward) {
  const usize n = panel.instruments;
  if (!n || d + 2 >= panel.dates || member.size() != panel.present.size() ||
      basis.size() != n * kBasisColumns || forward.size() != n)
    return co::Err(co::ErrorCode::InvalidArgument, "exposures: decision geometry");
  try {
    s.exposures.resize(n * kPriceExposureCount);
    s.ok.resize(n);
    s.rows.clear();
    s.rows.reserve(n);
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "exposures: allocation failed");
  }
  ATX_TRY_VOID(compute_price_exposures(panel, cfg, d, s.exposure, s.exposures, s.ok));
  const usize row = d * n;
  for (usize i = 0; i < n; ++i)
    if (member[row + i] != 0 && panel.present[row + i] != 0 && s.ok[i] != 0) s.rows.push_back(i);
  try {
    s.q.resize(s.rows.size() * kBasisColumns);
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "exposures: allocation failed");
  }
  ATX_TRY(const auto refusal, neutralization_basis(s.exposures, s.rows, cfg, s.neutralize, s.q));
  std::fill(basis.begin(), basis.end(), 0.0);
  if (refusal == BasisRefusal::None)
    for (usize r = 0; r < s.rows.size(); ++r)
      for (usize k = 0; k < kBasisColumns; ++k)
        basis[s.rows[r] * kBasisColumns + k] = s.q[r * kBasisColumns + k];
  ATX_TRY_VOID(session_interval_returns(panel, d + 2, s.exposure, forward));
  return co::Ok(ExposureDecision{refusal, s.rows.size()});
}

co::Status run_exposure_export(const ExposureExportConfig& cfg, std::ostream& progress) {
  try {
    if (cfg.role_path.empty() || !hash_valid(cfg.role_sha256) || cfg.output_directory.empty())
      return co::Err(co::ErrorCode::InvalidArgument,
                     "exposures: --role, a --role-sha256 SHA-256 and --output are required");
    const auto root = std::filesystem::path(cfg.output_directory);
    if (std::filesystem::exists(root))
      return co::Err(co::ErrorCode::AlreadyExists, "exposures: output must not exist");
    // The seal from the pinned manifest alone: every role session precedes score_end_ns
    // (read_strategy_role), so no payload of a role reaching the seal is ever opened.
    ATX_TRY(const auto manifest, pinned_manifest(cfg.role_path, cfg.role_sha256));
    if (!manifest.contains("score_end_ns") || !manifest.at("score_end_ns").is_number_integer() ||
        manifest.at("score_end_ns").get<i64>() > ed::kSealBeginNs)
      return sealed("the role's score window");
    const u64 dates = manifest.at("dates").get<u64>();
    const u64 names = manifest.at("instruments").get<u64>();
    if (!dates || dates > max_dates || !names || names > max_names)
      return co::Err(co::ErrorCode::InvalidArgument, "exposures: role geometry");
    const PriceExposureConfig price{};
    // Beside the role (charged by its reader): the presence copy, the session ring, the
    // per-name exposure, basis, forward and QR rows, and fixed slack.
    const u64 extra = dates * names + session_ring_bytes(price, static_cast<usize>(names)) +
                      names * 24 * sizeof(f64) + (16ULL << 20);
    if (cfg.max_working_bytes <= extra)
      return co::Err(co::ErrorCode::OutOfRange, "exposures: --max-bytes below the working set");
    ATX_TRY(auto role, ed::read_strategy_role(cfg.role_path, cfg.max_working_bytes - extra));
    if (role.manifest_sha256 != cfg.role_sha256)
      return co::Err(co::ErrorCode::InvalidArgument, "exposures: role manifest changed after pin");
    if (ed::is_sealed(role.session_keys.back())) return sealed("a role session");
    if (role.score_end < role.score_begin + 4)
      return co::Err(co::ErrorCode::InvalidArgument, "exposures: fewer than two scored decisions");
    const usize d_count = role.panel.dates(), n = role.panel.instruments();
    const usize begin = role.score_begin, end = role.score_end - 2;
    ATX_TRY(const auto close_id, role.panel.field_id("close"));
    ATX_TRY(const auto raw_id, role.panel.field_id("raw_close"));
    ATX_TRY(const auto volume_id, role.panel.field_id("volume"));
    std::vector<u8> present(d_count * n);
    for (usize t = 0; t < d_count; ++t)
      for (usize i = 0; i < n; ++i)
        present[t * n + i] = static_cast<u8>(role.panel.in_universe(t, i));
    const PriceExposureInput panel{d_count, n, role.panel.field_all(close_id),
                                   role.panel.field_all(raw_id), role.panel.field_all(volume_id),
                                   present};
    ExposureExportScratch scratch;
    enable_session_ring(scratch.exposure, panel); // each session logged once
    if (!std::filesystem::create_directory(root))
      return co::Err(co::ErrorCode::AlreadyExists, "exposures: output must not exist");
    std::ofstream basis_file(root / "basis.f64", std::ios::binary);
    std::ofstream forward_file(root / "forward_returns.f64", std::ios::binary);
    if (!basis_file || !forward_file)
      return co::Err(co::ErrorCode::IoError, "exposures: payload open");
    std::vector<f64> basis(n * kBasisColumns), forward(n);
    Json used_rows = Json::array(), refused = Json::array();
    for (usize d = begin; d < end; ++d) {
      ATX_TRY(const auto decision, export_decision(panel, role.decision_member, price, d, scratch,
                                                   basis, forward));
      write_f64(basis_file, basis);
      write_f64(forward_file, forward);
      const bool has_basis = decision.refusal == BasisRefusal::None;
      used_rows.push_back(has_basis ? decision.used_rows : usize{0});
      if (!has_basis)
        refused.push_back(Json{{"decision_index", d},
                               {"reason", basis_refusal_id(decision.refusal)},
                               {"used_rows", decision.used_rows}});
    }
    basis_file.close();
    forward_file.close();
    if (!basis_file || !forward_file)
      return co::Err(co::ErrorCode::IoError, "exposures: payload write");
    ATX_TRY(auto basis_sha, co::sha256_file((root / "basis.f64").string()));
    ATX_TRY(auto forward_sha, co::sha256_file((root / "forward_returns.f64").string()));
    const usize decisions = end - begin;
    const u64 cells = u64{decisions} * n;
    const auto refused_count = refused.size();
    const Json out{
        {"schema", export_schema}, {"status", "complete"}, {"contract", "K2"},
        {"role_manifest_sha256", cfg.role_sha256},
        {"research_window", std::string(ed::kResearchWindowId)},
        {"ids_sha256", manifest.at("files").at("ids.u64").at("sha256")},
        {"dates", d_count}, {"instruments", n}, {"decision_begin", begin},
        {"decision_end_exclusive", end}, {"decisions", decisions},
        {"first_decision_session_ns", role.session_keys[begin]},
        {"last_decision_session_ns", role.session_keys[end - 1]},
        {"last_label_session_ns", role.session_keys[end + 1]},
        {"price_risk", {{"beta_window", price.beta_window}, {"vol_window", price.vol_window},
                        {"adv_window", price.adv_window},
                        {"min_return_pairs", price.min_return_pairs},
                        {"min_names", price.min_names}, {"clip_z", price.clip_z}}},
        {"semantics", export_semantics}, {"fitter_context_semantics", fitter_context_semantics},
        {"used_rows", std::move(used_rows)}, {"refused", std::move(refused)},
        {"files", {{"basis.f64", file_entry(basis_sha, cells * kBasisColumns * sizeof(f64),
                                            Json::array({decisions, n, kBasisColumns}),
                                            basis_layout)},
                   {"forward_returns.f64", file_entry(forward_sha, cells * sizeof(f64),
                                                      Json::array({decisions, n}),
                                                      forward_layout)}}}};
    {
      std::ofstream file(root / "manifest.json", std::ios::binary);
      file << out.dump(2) << '\n';
      file.close();
      if (!file) return co::Err(co::ErrorCode::IoError, "exposures: manifest write");
    }
    progress << "exposures: " << decisions << " decisions x " << n << " names, " << refused_count
             << " refused -> " << root.string() << '\n';
    return co::Ok();
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "exposures: allocation failed");
  } catch (const std::exception& e) {
    return co::Err(co::ErrorCode::InvalidArgument, std::string("exposures: ") + e.what());
  }
}

int dispatch_exposures(int argc, char** argv, std::ostream& out, std::ostream& err) {
  try {
    ExposureExportConfig cfg;
    std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
      const std::string key = argv[i];
      if (key == "--help") {
        out << "exposures --role PATH/manifest.json --role-sha256 SHA --output NEWDIR "
               "[--max-bytes 1400000000]\n"
               "Writes the fitter's price-risk context (contract K2) for the role's decisions "
               "[score_begin, score_end - 2): basis.f64 (decisions x names x 4, the orthonormal "
               "Q of [1, z_beta, z_vol, z_log_adv] over the used names), forward_returns.f64 "
               "(decisions x names, interval d + 2, NaN when invalid), manifest.json last.\n";
        return 0;
      }
      if (!seen.insert(key).second || i + 1 >= argc)
        throw std::invalid_argument("duplicate/missing flag");
      const std::string value = argv[++i];
      if (key == "--role") cfg.role_path = value;
      else if (key == "--role-sha256") cfg.role_sha256 = value;
      else if (key == "--output") cfg.output_directory = value;
      else if (key == "--max-bytes") {
        u64 x = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), x);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
          throw std::invalid_argument("invalid integer");
        cfg.max_working_bytes = x;
      } else throw std::invalid_argument("unknown flag: " + key);
    }
    if (cfg.role_path.empty() || cfg.role_sha256.empty() || cfg.output_directory.empty())
      throw std::invalid_argument("--role, --role-sha256 and --output are required");
    const auto status = run_exposure_export(cfg, out);
    if (!status) { err << status.error().to_string() << '\n'; return 1; }
    return 0;
  } catch (const std::exception& e) { err << "exposures: " << e.what() << '\n'; return 2; }
}
} // namespace atx::impl::strategy
