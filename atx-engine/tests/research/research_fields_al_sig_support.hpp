#pragma once

// Test support of lane AL-SIG's gtests (atx-engine-research-fields-tests): the synthetic stage
// fixture (tests/fixtures/research_fields_al_sig, written with its oracle by
// make_al_sig_fixture.py), calendar helpers and the leak probe's two clocks.

#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/types.hpp"
#include "atx/engine/research/fields/build_spec.hpp"
#include "atx/engine/research/fields/clock.hpp"
#include "atx/engine/research/fields/sources/stage.hpp"
#include "research/research_fields_registry_support.hpp"
#include "research/research_fields_test_support.hpp"

namespace atx::engine::research::fields::test {

inline i64 civil_day(i64 y, u32 m, u32 d) { return days_from_civil(y, m, d); }

// The AL-SIG fixture directory (beside the slice-1 fixture).
inline fs::path al_sig_dir() { return fixture_dir().parent_path() / "research_fields_al_sig"; }

inline nlohmann::json al_sig_counts() {
  return nlohmann::json::parse(read_bytes(al_sig_dir() / "expected" / "counts.json"));
}

// A fixture stage request pinned by its manifest bytes.
inline StageRequest al_sig_stage(const std::string &name) {
  const fs::path dir = al_sig_dir() / name;
  return StageRequest{dir, sha256_of(read_bytes(dir / "manifest.json"))};
}

// The committed field registry (atx-engine/tools/field_registry.json).
inline fs::path committed_registry_path() {
  return fixture_dir().parent_path().parent_path().parent_path() / "tools" /
         "field_registry.json";
}

// Visibility marks of a day axis: the house rule (the previous session's 22:00 UTC mark) and the
// planted leak a probe must catch (the session's own mark: a same-session read).
inline std::vector<i64> honest_marks(const std::vector<i64> &days) {
  return previous_session_marks(days);
}

inline std::vector<i64> leaky_marks(const std::vector<i64> &days) {
  std::vector<i64> out;
  out.reserve(days.size());
  for (const i64 d : days) {
    out.push_back(d * kDayNs + kSessionMarkNs);
  }
  return out;
}

inline f64 nan_value() { return std::numeric_limits<f64>::quiet_NaN(); }

} // namespace atx::engine::research::fields::test
