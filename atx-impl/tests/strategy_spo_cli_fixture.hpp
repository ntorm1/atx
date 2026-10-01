#pragma once

// The `nav` command-line inputs of the spo tests (strategy_spo_test.cpp, strategy_spo_v3_test.cpp):
// a fixture Role written as the pinned combined blend and price role the verb reads (the layout
// of strategy_nav_replay_test.cpp's write_artifact), and the JSON file helpers their checks use.
// Moved here unchanged from strategy_spo_test.cpp (platform v8 FIX-4b, review R6B-S-1).

#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "atx/core/types.hpp"
#include "strategy_spo_fixture.hpp"

namespace atx::impl::strategy::spo::fixture {

// Writes `value` (two-space indent, trailing newline) and returns the file's SHA-256.
inline std::string write_json_file(const std::filesystem::path& path,
                                   const nlohmann::json& value) {
  std::ofstream out(path, std::ios::binary);
  out << value.dump(2) << '\n';
  out.close();
  if (!out) throw std::runtime_error("fixture JSON write");
  return atx::core::sha256_file(path.string()).value();
}
inline nlohmann::json read_json_file(const std::filesystem::path& path) {
  std::ifstream in(path);
  nlohmann::json j;
  in >> j;
  return j;
}
// The pinned combined blend and price role of `r` (the layout of strategy_nav_replay_test.cpp's
// write_artifact).
struct RunInputs {
  std::string combined, combined_sha256, role, role_sha256;
};
inline RunInputs write_run_inputs(const std::filesystem::path& dir, const Role& r) {
  using Json = nlohmann::json;
  std::vector<atx::u8> finite(r.signal.size());
  atx::u64 finite_count = 0, members = 0;
  for (atx::usize k = 0; k < finite.size(); ++k) {
    finite[k] = static_cast<atx::u8>(std::isfinite(r.signal[k]));
    finite_count += finite[k]; members += r.member[k];
  }
  Json files;
  files["train_combined.f64"] = write_payload(dir / "train_combined.f64", r.signal);
  files["train_combined_member.u8"] = write_payload(dir / "train_combined_member.u8", r.member);
  files["train_combined_finite.u8"] = write_payload(dir / "train_combined_finite.u8", finite);
  files["train_combined_sessions.i64"] =
      write_payload(dir / "train_combined_sessions.i64", r.sessions);
  files["train_combined_ids.u64"] = write_payload(dir / "train_combined_ids.u64", r.ids);
  const std::string pin(64, 'a');
  Json manifest{{"schema", "atx.dsl-combined-signal/v1"}, {"status", "complete"},
      {"role", "train"}, {"layout", "date-major-little-endian"}, {"dates", r.d},
      {"instruments", r.n}, {"score_begin", 0}, {"score_end", r.d},
      {"role_manifest_sha256", pin}, {"source_sha256", pin}, {"library_sha256", pin},
      {"train_manifest_sha256", pin}, {"run_recipe_sha256", pin},
      {"orientation_candidates_sha256", pin}, {"orientations_artifact_sha256", nullptr},
      {"role_window_required", true},
      {"signal_semantics", "exact-pre-target-composition;equal-family/equal-within;"
                           "missing-or-unoriented-neutral-fixed-denominator"},
      {"member_semantics", "decision-member-and-source-present-and-finite-positive-close;"
                           "independent-of-component-coverage"},
      {"finite_semantics",
       "one-iff-saved-f64-is-finite;nonmembers-NaN;zero-is-valid-neutral-signal"},
      {"actual_trades_or_returns", false}, {"finite_cells", finite_count},
      {"member_cells", members}, {"files", std::move(files)}};
  Json role_files;
  role_files["sessions.i64"] = write_payload(dir / "sessions.i64", r.sessions);
  role_files["ids.u64"] = write_payload(dir / "ids.u64", r.ids);
  role_files["close.f64"] = write_payload(dir / "close.f64", r.close);
  role_files["raw_close.f64"] = write_payload(dir / "raw_close.f64", r.raw);
  role_files["present.u8"] = write_payload(dir / "present.u8", r.present);
  role_files["member.u8"] = write_payload(dir / "member.u8", r.member);
  role_files["volume.f64"] = write_payload(dir / "volume.f64", r.volume);
  const Json role{{"schema", "atx.recent-research-role/v1"}, {"status", "complete"},
      {"source_sha256", pin}, {"instrument_namespace", "spiderrock.securityID"},
      {"close_basis", "f64(raw-f32-close)*f64-cumulReturnFactor"},
      {"volume_basis", "raw-share-volume"},
      {"clock_recipe", "modeled-session+22h-mark+23h-decision-v1"},
      {"common_stock_verified", false}, {"historical_vintage_verified", false},
      {"dates", r.d}, {"instruments", r.n}, {"score_begin", 0}, {"score_end", r.d},
      {"files", std::move(role_files)}};
  RunInputs in;
  in.role = (dir / "role.json").string();
  in.role_sha256 = write_json_file(in.role, role);
  manifest["role_manifest_sha256"] = in.role_sha256;
  in.combined = (dir / "train_combined.json").string();
  in.combined_sha256 = write_json_file(in.combined, manifest);
  return in;
}
} // namespace atx::impl::strategy::spo::fixture
