#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "atx/ui/vol_surface_source.hpp"

namespace atx::vol {
class OptionChain;
class PricerFitter;
} // namespace atx::vol

namespace atx::ui {

struct OpraSourceConfig {
  std::string path{"data/spy_opra_cbbo1m_2026-06-05T1955Z.parquet"};
  std::string symbol{"SPY"};
  std::string snapshot_iso{"2026-06-05T19:55:00Z"};
  std::string initial_expiry;
  double rate{0.043};
};

// Databento OPRA adapter for the generic VolSurfaceSource contract. Despite
// the default fixture being SPY, neither the implementation nor its consumers
// assume a particular underlying.
class OpraVolSurface final : public VolSurfaceSource {
public:
  OpraVolSurface();
  ~OpraVolSurface() override;
  OpraVolSurface(OpraVolSurface &&) noexcept;
  OpraVolSurface &operator=(OpraVolSurface &&) noexcept;
  OpraVolSurface(const OpraVolSurface &) = delete;
  OpraVolSurface &operator=(const OpraVolSurface &) = delete;

  [[nodiscard]] bool load(const OpraSourceConfig &config);
  [[nodiscard]] bool select_expiry(std::size_t index) override;

  [[nodiscard]] bool ready() const noexcept override;
  [[nodiscard]] const std::string &error() const noexcept override;
  [[nodiscard]] const OpraSourceConfig &config() const noexcept;
  [[nodiscard]] const SurfaceSourceInfo &source_info() const noexcept override;
  [[nodiscard]] const std::vector<ExpiryInfo> &expiries() const noexcept override;
  [[nodiscard]] std::size_t selected_expiry() const noexcept override;
  [[nodiscard]] const VolCurveSlice &slice() const noexcept override;
  [[nodiscard]] const SurfaceDiagnostics &diagnostics() const noexcept override;
  [[nodiscard]] std::size_t contract_count() const noexcept override;
  [[nodiscard]] std::size_t dropped_count() const noexcept override;
  [[nodiscard]] double fit_milliseconds() const noexcept override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Source compatibility for code written against the first SPY-only prototype.
using SpyOpraSurface [[deprecated("use OpraVolSurface")]] = OpraVolSurface;

} // namespace atx::ui

