#pragma once

// Research-only observation, compiled out unless the explicit CMake capture
// option is on. The environment variable selects an output file. Capture adds
// I/O and serialization: its clocks are not performance evidence.
#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <stdexcept>

#include "pricing/american_iv_premium.hpp"

namespace atx::vol::research {
inline std::uint64_t next_cold_seed_batch() noexcept {
  static std::atomic<std::uint64_t> next{0};
  return next.fetch_add(1, std::memory_order_relaxed);
}

inline void capture_cold_seed(std::uint64_t batch, std::size_t index,
    const IvNativeBatchRequest &r, double budget, double sigma,
    const std::optional<AlOpts> &opts, bool accepted) {
  struct Writer {
    std::mutex mutex;
    std::ofstream output;
    Writer() {
      std::array<char,4096> path{};
      std::size_t length = 0;
      if (getenv_s(&length,path.data(),path.size(),"ATX_VOL_COLD_SEED_CAPTURE") != 0)
        throw std::runtime_error("Invalid cold seed capture path");
      if (length == 0 || path[0] == '\0') return;
      output.open(path.data(), std::ios::out | std::ios::trunc);
      if (!output) throw std::runtime_error("Cannot open cold seed capture");
      output << "batch,index,S,K,T,r,q,side,target,budget,tol,sigma,accepted,"
                "opts_explicit,n_collocation,n_quadrature,n_quad_price,max_newton_iter,opts_tol,profile\n";
      output << std::setprecision(17);
    }
  };
  static Writer writer;
  const std::lock_guard lock(writer.mutex);
  if (!writer.output.is_open()) return;
  auto a = opts.value_or(al_fast_profile(std::nullopt));
  if (!opts) a.profile = AlProfile::Reference;
  writer.output << batch << ',' << index << ',' << r.S << ',' << r.K << ',' << r.T
      << ',' << r.r << ',' << r.q << ',' << static_cast<int>(r.side) << ',' << r.price
      << ',' << budget << ',' << r.tol << ',' << sigma << ',' << accepted << ','
      << opts.has_value() << ',' << a.n_collocation << ',' << a.n_quadrature << ','
      << a.n_quad_price << ',' << a.max_newton_iter << ',' << a.tol << ','
      << static_cast<int>(a.profile) << '\n';
  if (!writer.output) throw std::runtime_error("Cannot write cold seed capture");
}
} // namespace atx::vol::research
