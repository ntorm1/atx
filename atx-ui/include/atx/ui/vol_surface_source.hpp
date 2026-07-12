#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace atx::ui {

enum class UiFitQualityMode { Latency, Balanced, Accuracy };

// Stable UI-facing view models. Feed adapters translate domain objects into
// these types so panels do not depend on OPRA, a particular symbol, or the
// internal representation used by atx-vol.
struct SurfaceSourceInfo {
  std::string provider;
  std::string feed;
  std::string path;
  std::string symbol;
  std::string snapshot_iso;
};

struct ExpiryInfo {
  std::string iso_date;
  double years{0.0};
  double forward{0.0};
  double atm_vol{0.0};
  double carry{0.0};
  double total_variance{0.0};
  double forward_variance{0.0};
  std::size_t strike_count{0};
};

struct VolCurvePoint {
  double z{0.0};
  double strike{0.0};
  double model_iv{0.0};
};

struct VolQuotePoint {
  double z{0.0};
  double strike{0.0};
  char side{'C'};
  double bid_price{0.0};
  double mid_price{0.0};
  double ask_price{0.0};
  double theoretical_price{0.0};
  double bid_iv{0.0};
  double ask_iv{0.0};
  double mid_iv{0.0};
  double model_iv{0.0};
  double delta{0.0};
  double gamma{0.0};
  double theta{0.0};
  double vega{0.0};
};

struct SurfaceDiagnostics {
  double worst_in_band{0.0};
  double mean_in_band{0.0};
  double mean_rmse_vol{0.0};
  std::size_t fitted_slices{0};
  std::size_t fitted_quotes{0};
  std::size_t calendar_violations{0};
  bool calendar_arb_free{false};
  std::string risk_state;
  std::string quality_mode;
  std::string risk_model;
  std::string mark_model;
  std::uint64_t candidate_generation{0};
  std::uint64_t served_generation{0};
  bool using_fallback{false};
  bool carry_confident{false};
  bool inversion_certified{false};
  std::size_t butterfly_violations{0};
  std::size_t inversion_fallbacks{0};
  double carry_dispersion{0.0};
  double carry_leave_one_out{0.0};
  double validation_milliseconds{0.0};
};

struct VolCurveSlice {
  std::string symbol;
  std::string snapshot_iso;
  std::string expiry_iso;
  std::string model_name;
  double spot{0.0};
  double forward{0.0};
  double years{0.0};
  double rate{0.0};
  double carry{0.0};
  double atm_vol{0.0};
  double fraction_in_band{0.0};
  double rmse_iv{0.0};
  double max_abs_error{0.0};
  std::size_t observations{0};
  std::vector<VolCurvePoint> curve;
  std::vector<VolCurvePoint> market_mark_curve;
  std::vector<VolQuotePoint> quotes;
};

// Read-only source contract shared by every volatility panel. A future live
// feed, replay source, scenario surface, or alternate vendor can implement this
// interface without changing workspace components.
class VolSurfaceSource {
public:
  virtual ~VolSurfaceSource() = default;

  [[nodiscard]] virtual bool select_expiry(std::size_t index) = 0;
  [[nodiscard]] virtual bool ready() const noexcept = 0;
  [[nodiscard]] virtual const std::string &error() const noexcept = 0;
  [[nodiscard]] virtual const SurfaceSourceInfo &source_info() const noexcept = 0;
  [[nodiscard]] virtual const std::vector<ExpiryInfo> &expiries() const noexcept = 0;
  [[nodiscard]] virtual std::size_t selected_expiry() const noexcept = 0;
  [[nodiscard]] virtual const VolCurveSlice &slice() const noexcept = 0;
  [[nodiscard]] virtual const SurfaceDiagnostics &diagnostics() const noexcept = 0;
  [[nodiscard]] virtual std::size_t contract_count() const noexcept = 0;
  [[nodiscard]] virtual std::size_t dropped_count() const noexcept = 0;
  [[nodiscard]] virtual double fit_milliseconds() const noexcept = 0;
  [[nodiscard]] virtual UiFitQualityMode quality_mode() const noexcept = 0;
  [[nodiscard]] virtual bool set_quality_mode(UiFitQualityMode mode) = 0;
};

} // namespace atx::ui
