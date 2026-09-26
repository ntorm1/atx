#pragma once

#include <iosfwd>
#include <string>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::impl::strategy {
// Fixed research scenario, not observed spread/borrow or locate availability.
// One role is retained at a time. Admission is a conservative payload/workspace
// bound, not a guarantee about process RSS or other applications' memory.
struct RunnerConfig {
  std::string library_path, train_manifest, validation_manifest, holdout_manifest;
  std::string library_sha256, train_sha256, validation_sha256, holdout_sha256;
  std::string cash_claims_path, cash_claims_sha256; // paired optional reconstructed-publication input
  std::string stock_transitions_path, stock_transitions_sha256; // paired optional reconstructed stock delivery
  std::string output_directory;
  atx::u64 max_working_bytes{512ULL << 20};
  atx::u64 seed{42}; // declared deterministic recipe; no random search is performed
  atx::usize min_names{20};
  atx::usize liquidity_window{63};
  atx::f64 initial_nav{1'000'000'000.0};
  atx::f64 full_spread_bps{10.0};
  atx::f64 commission_bps{1.0};
  atx::f64 annual_borrow_bps{300.0};
  atx::f64 impact_y{0.6};
  atx::f64 max_participation{0.01};
};
// Creates an exclusive output directory. TRAIN signs are selected on the
// predeclared primary variant; fixed family weights/signs are reused unchanged.
// Missing contributions are neutral zero with fixed denominators. Unscorable
// candidates fail the run. HOLDOUT is opened only when explicitly supplied.
[[nodiscard]] atx::core::Status run(const RunnerConfig&, std::ostream& progress);
[[nodiscard]] int dispatch(int argc, char** argv, std::ostream& out, std::ostream& err);
} // namespace atx::impl::strategy
