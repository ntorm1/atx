#pragma once

// RiskFrame — the per-state Greek frame of a PreparedRiskBook: one SoA row per
// unique option in PREPARED order, per-uid scalars, per-share marks, and the
// counters that say how the frame was produced.
//
// FROZEN API CONTRACT (sprint 2026-10-24; see risk_types.hpp banner).
//
// A frame is computed ONCE per MarketState by a RiskKernel and is then both the
// shifted side of the previous explain step and the base side of the next
// (RiskStream). Columns are plain vectors so a caller can `reserve` once and reuse
// the frame across states with no allocation.
//
// Conventions (American, spot-based; per SHARE of the underlying, unscaled):
//   price = American mark        delta = dP/dS      gamma = d2P/dS2
//   vega  = dP/dsigma            theta = dP/dt (calendar; = -dP/dT)
//   rho   = dP/dr                vanna = d2P/dS dsigma   volga = d2P/dsigma2
//   charm = d2P/dS dt
// An axis not requested in RiskNeeds is 0.0 (never NaN). A row whose status != Ok
// has NaN in every numeric column and must not be read by a consumer.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/risk/risk_types.hpp"

namespace atx::vol {

struct FrameCounters {
  std::uint64_t families_built{0};     // sigma families prepared for this frame
  std::uint64_t carry_sets_built{0};   // rho/phi carry-bump sets prepared
  std::uint64_t family_rows{0};        // rows served by a family
  std::uint64_t cold_rows{0};          // rows served cold (fallback or cold kernel)
  std::uint64_t exercised_rows{0};
  std::uint64_t refused_rows{0};
  std::array<std::uint32_t, kRiskLaneStatusCount> status_count{}; // by RiskLaneStatus
  double evaluate_seconds{0.0};        // wall clock of the kernel evaluate call
  double family_build_seconds{0.0};    // of which: family (+carry) preparation
};

struct RiskFrame {
  std::int64_t ts_ns{0};
  std::uint64_t book_logical_id{0};
  RiskNeeds needs{};

  // Per unique option, prepared order (length n_unique).
  std::vector<double> T;
  std::vector<double> sigma;
  std::vector<double> forward;
  std::vector<double> q_eff;
  std::vector<double> rate;
  std::vector<double> price;
  std::vector<double> delta;
  std::vector<double> gamma;
  std::vector<double> vega;
  std::vector<double> theta;
  std::vector<double> rho;
  std::vector<double> vanna;
  std::vector<double> volga;
  std::vector<double> charm;
  std::vector<double> vendor_price; // NaN unless VendorParityAvailable
  std::vector<double> vendor_delta; // NaN unless VendorParityAvailable
  std::vector<RiskRoute> route;
  std::vector<RiskFlags> flags;
  std::vector<RiskLaneStatus> status;

  // Per uid slot (PreparedRiskBook::uids() order).
  std::vector<double> uid_spot;
  std::vector<double> uid_flat_rate;
  std::vector<RiskLaneStatus> uid_status; // ModelUnavailable when absent from the state

  // Per share leg (input order): mark = spot of its uid, or NaN with status.
  std::vector<double> share_mark;
  std::vector<RiskLaneStatus> share_status;

  FrameCounters counters{};

  [[nodiscard]] std::size_t size() const noexcept { return price.size(); }
  // Size every column; existing contents are not preserved. Idempotent; grow-only
  // capacity so a steady-state re-reserve allocates nothing.
  void reserve(std::size_t n_unique, std::size_t n_uids, std::size_t n_shares);
  // Reset every numeric column to NaN / status to Refused / route None, keeping size.
  void clear_values() noexcept;
};

} // namespace atx::vol
