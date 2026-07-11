#include "oracle_pde_golden.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

namespace atx::vol::test {
namespace {

namespace fs = std::filesystem;

// ATX_VOL_ORACLE_REGEN is read with _dupenv_s under MSVC/clang-cl: plain
// std::getenv trips /WX (-Wdeprecated-declarations) here. Matches the
// codebase env-read pattern (see tests/support/bench_gate.hpp).
bool regen_enabled() noexcept {
#if defined(_MSC_VER)
  char* e = nullptr;
  std::size_t n = 0;
  if (::_dupenv_s(&e, &n, "ATX_VOL_ORACLE_REGEN") != 0 || e == nullptr) {
    return false;
  }
  std::free(e);
  return true;
#else
  return std::getenv("ATX_VOL_ORACLE_REGEN") != nullptr;
#endif
}

// The TSV sits next to the test sources; probe the same way opra_fixture.hpp
// probes data/ so it works from any ctest working directory.
fs::path golden_path() {
  for (const char* p : {"../../../atx-vol/tests/support/oracle_pde_golden.tsv",
                        "atx-vol/tests/support/oracle_pde_golden.tsv",
                        "../atx-vol/tests/support/oracle_pde_golden.tsv",
                        "C:/atx/atx-vol/tests/support/oracle_pde_golden.tsv"}) {
    if (fs::exists(p)) return fs::path{p};
  }
  return fs::path{"C:/atx/atx-vol/tests/support/oracle_pde_golden.tsv"};
}

std::string key_of(double S, double K, double T, double sigma, double r,
                   double q, Side side, const OraclePdeOpts& o) {
  char buf[256];
  std::snprintf(buf, sizeof buf,
                "%.17g|%.17g|%.17g|%.17g|%.17g|%.17g|%d|%d|%d|%.17g|%.17g", S,
                K, T, sigma, r, q, static_cast<int>(side), o.n_t, o.n_x,
                o.s_min_mult, o.s_max_mult);
  return buf;
}

std::unordered_map<std::string, double>& table() {
  static std::unordered_map<std::string, double> t = [] {
    std::unordered_map<std::string, double> m;
    std::ifstream in(golden_path());
    std::string k;
    double v;
    while (in >> k >> v) m.emplace(std::move(k), v);
    return m;
  }();
  return t;
}

}  // namespace

double oracle_pde_golden(double S, double K, double T, double sigma, double r,
                         double q, Side side, const OraclePdeOpts& opts) {
  static std::mutex mu;
  const std::string k = key_of(S, K, T, sigma, r, q, side, opts);
  {
    std::lock_guard<std::mutex> lock(mu);
    auto it = table().find(k);
    if (it != table().end()) return it->second;
  }
  const double live = oracle_pde_american(S, K, T, sigma, r, q, side, opts);
  if (regen_enabled()) {
    std::lock_guard<std::mutex> lock(mu);
    table().emplace(k, live);
    std::ofstream out(golden_path(), std::ios::app);
    out.precision(17);
    out << k << '\t' << std::scientific << live << '\n';
  } else {
    ADD_FAILURE() << "oracle_pde_golden miss for key " << k
                  << " — regenerate: set ATX_VOL_ORACLE_REGEN=1 and rerun "
                     "this test in the release build, then commit "
                     "atx-vol/tests/support/oracle_pde_golden.tsv";
  }
  return live;
}

}  // namespace atx::vol::test
