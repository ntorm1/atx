#pragma once
#include <iosfwd>
#include <string>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
namespace atx::impl::strategy {
struct IcRunnerConfig {
  std::string library_path, library_sha256;
  std::string train_manifest, train_sha256;
  std::string validation_manifest, validation_sha256;
  std::string output_directory;
  atx::u64 max_working_bytes{512ULL<<20};
  atx::usize min_names{20},min_dates{128};
  atx::usize workers{1}; // 1 is the existing serial baseline; explicit2..4 share one VM pool
  bool plan_only{false}; // pinned metadata/DSL compilation only; no role payloads
};
// IC-only research; no book, surfaces, fees, Sharpe, stock events or holdout.
// TRAIN21h sample rank-IC fits signs; validation uses frozen signs; screening remains diagnostic for the fixed blend.
// One role, one label cache and one evaluated DSL signal are retained at a time.
[[nodiscard]] atx::core::Status run_ic(const IcRunnerConfig&,std::ostream& progress);
[[nodiscard]] int dispatch_ic(int argc,char** argv,std::ostream& out,std::ostream& err);
} // namespace atx::impl::strategy
