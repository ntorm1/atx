#pragma once

// Per-expiry (log-moneyness, sigma) calibration record at the expiry's exact
// carry — pricing hot-path audit 2026-10-16, item 2.
//
// ## What it replaces, and why
//
// The served cached American price today is Black-76 plus ONE per-underlier
// CorrectionCache: a 16 x 8 x 12 Chebyshev tensor over (k, T, sigma) baked at a
// single representative carry q_rep (session.cpp build_session_caches). Every
// cached price walks all 1,536 coefficients, and every expiry whose q_eff differs
// from q_rep inherits the carry mismatch (the A9 note in american.cpp). On the
// audit's 2,000-contract chain that route misses the cold Andersen-Lake reference
// by up to ~$0.58 in price.
//
// An ExpiryRecord fixes the expiry: T, r and q are EXACT for that expiry, the T
// axis disappears, and one record is a 16 x 12 coefficient plane per side
// (192 coefficients, 1.5 KB). A cached price is one 192-term 2-D Clenshaw plus
// the Black-76 leg.
//
// ## What is recorded
//
// The forward-normalized early-exercise premium c(k, sigma) = premium / F of the
// Andersen-Lake decomposition American = European + premium, sampled at
// Chebyshev nodes with F = 1 (spot e^{-(r-q)T}, strike e^k). By homogeneity
// P(S, K) = F p(k) at fixed (T, sigma, r, q), so one record prices every (spot,
// strike) pair of the expiry: k = ln(K / F), F = S e^{(r-q)T}.
//
// ## The exercise edge (why this is not a plain tensor)
//
// The American price has a second-derivative jump at the current exercise
// boundary; a plain tensor that straddles it converges only algebraically
// (O(n^-2)) there. The record solves the boundary at each sigma node anyway, so
// it records the exercise edge k*(sigma) (puts: exercised for k >= k*; calls: for
// k <= k*) as a 12-term Chebyshev series and maps the moneyness axis of each
// sigma row onto the CONTINUATION side only, where the premium is smooth. Queries
// past the edge return intrinsic value (delta +/-1, gamma = vega = theta = 0) —
// the same convention american_greeks_al uses in its exercise region.
//
// ## Certification
//
// build() evaluates the finished record against the cold accurate Andersen-Lake
// solver (nullopt preset, a fresh boundary per check sigma) on an off-node grid
// — every sigma midpoint between nodes plus both box ends, crossed with every
// moneyness midpoint on the continuation side, the box ends, and points inside
// the exercise region — and stores the maximum absolute price error in forward
// units. Multiply by F for dollars. This is a measured bound on that grid, not a
// proof between grid points.
//
// ## Carry and dividends
//
// Continuous carry only: `ExpiryCarry{rate, carry}` is the expiry's exact r and
// q_eff (borrow plus dividend yield). `ExpiryCarry::dividends` is a typed hook
// for DATED cash dividends; build() refuses a non-empty list (NotImplemented).
// What is missing: a dated dividend breaks spot homogeneity (P(S,K) is no longer
// F p(k) once a fixed cash amount drops the spot), so the record needs a third
// coordinate (the dividend-to-spot ratio) or a per-spot-window rebuild, and the
// cold reference for those nodes is the Vellekoop-Nieuwenhuis lattice
// (american_discrete_div_price), not Andersen-Lake. Neither is built here.
//
// ## Storage and threading
//
// Fixed-size arrays only: an ExpiryRecord owns no heap memory, and every query
// is noexcept and allocation-free. ExpiryRecordBank reserves its capacity once
// at construction and refuses inserts past it, so a bank never reallocates while
// it is read. A built record or bank is immutable and safe for concurrent reads.
//
// Tier-B: public and includable, deliberately outside the frozen umbrella.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/api/core/types.hpp"       // Side, Result, Status
#include "atx/vol/api/pricing/american.hpp" // AlOpts, CashDividend

namespace atx::vol {

// Carry to ONE expiry. `dividends` is the dated-cash-dividend hook described in
// the file header: it must be empty today.
struct ExpiryCarry {
  double rate = 0.0;  // continuous r to the expiry
  double carry = 0.0; // continuous q_eff to the expiry (borrow + dividend yield)
  std::span<const CashDividend> dividends{};
};

struct ExpiryRecordSpec {
  double expiry = 0.0; // T, year fraction, > 0
  ExpiryCarry carry{};
  Side side = Side::Put;
  double k_min = 0.0; // log-moneyness box ln(K / F); k_min < k_max
  double k_max = 0.0;
  double sigma_min = 0.0; // 0 < sigma_min < sigma_max
  double sigma_max = 0.0;
  // Scheme the sampling boundaries are solved with; nullopt = the ACCURATE preset.
  std::optional<AlOpts> sample_opts = std::nullopt;
  // Run the cold-solver certification (about one accurate boundary solve per
  // check sigma). When false, certified_max_abs_error_fwd() is NaN.
  bool certify = true;
};

// Price and sensitivities at one (spot, strike, sigma) from a record. theta is the
// calendar dV/dt per year from the continuation-region PDE
// (theta = r V - (r - q) S delta - 0.5 sigma^2 S^2 gamma), the american_greeks_al
// definition — the record has no T axis to differentiate.
struct RecordGreeks {
  double price = 0.0;
  double delta = 0.0;
  double gamma = 0.0;
  double vega = 0.0;
  double theta = 0.0;
  bool exercised = false; // spot in the exercise region: intrinsic, theta 0
  bool in_box = true;     // (k, sigma) inside the record box; outside it clamps
};

class ExpiryRecord {
public:
  static constexpr std::uint16_t kNk = 16; // moneyness nodes
  static constexpr std::uint16_t kNs = 12; // sigma nodes

  // An unpopulated record: every query returns NaN.
  ExpiryRecord() = default;

  // Build (and by default certify) one record.
  //
  // @return InvalidArgument — non-finite / non-positive T, an empty or inverted
  //                           box, sigma_min <= 0, or non-finite carry
  //         NotImplemented  — non-empty `carry.dividends` (dated dividends are not
  //                           supported yet), the double-continuation regime, or
  //                           a sigma node whose boundary cannot be solved
  [[nodiscard]] static Result<ExpiryRecord> build(const ExpiryRecordSpec &spec);

  [[nodiscard]] bool populated() const noexcept { return populated_; }
  [[nodiscard]] Side side() const noexcept { return side_; }
  [[nodiscard]] double expiry() const noexcept { return T_; }
  [[nodiscard]] double rate() const noexcept { return r_; }
  [[nodiscard]] double carry() const noexcept { return q_; }
  [[nodiscard]] double k_min() const noexcept { return k_min_; }
  [[nodiscard]] double k_max() const noexcept { return k_max_; }
  [[nodiscard]] double sigma_min() const noexcept { return s_min_; }
  [[nodiscard]] double sigma_max() const noexcept { return s_max_; }
  // e^{(r-q)T}: F = S * forward_factor().
  [[nodiscard]] double forward_factor() const noexcept { return growth_; }
  // True when early exercise is possible in this regime (put r > 0, call q > 0).
  [[nodiscard]] bool american() const noexcept { return american_; }
  // True when the exercise edge crosses the moneyness box for some sigma.
  [[nodiscard]] bool has_exercise_edge() const noexcept { return has_edge_; }

  // Max |record price - cold accurate AL price| / F over the certification grid,
  // and the number of grid points it covers. NaN / 0 when not certified.
  [[nodiscard]] double certified_max_abs_error_fwd() const noexcept { return cert_err_; }
  [[nodiscard]] std::uint32_t certified_points() const noexcept { return cert_pts_; }

  // Finite point inside the closed (k, sigma) box of a populated record.
  [[nodiscard]] bool contains(double k_log, double sigma) const noexcept;

  // Exercise edge k*(sigma) (unclamped). +inf (put) / -inf (call) when the
  // regime has no early exercise.
  [[nodiscard]] double exercise_edge(double sigma) const noexcept;

  // Forward-normalized premium c(k, sigma) >= 0 on the continuation side; 0 in
  // the exercise region. Out-of-box arguments clamp to the box.
  [[nodiscard]] double premium_fwd(double k_log, double sigma) const noexcept;

  // American price at (S, K, sigma): intrinsic in the exercise region, else
  // max(Black-76 + F c, intrinsic, Black-76). NaN for non-positive S/K/sigma or an
  // unpopulated record. One 192-term Clenshaw plus the Black-76 leg.
  [[nodiscard]] double price(double S, double K, double sigma) const noexcept;

  // Price and vega only (the spot-ladder builder's pair). *vega_out receives
  // dPrice/dsigma at fixed (S, K).
  [[nodiscard]] double price_and_vega(double S, double K, double sigma,
                                      double *vega_out) const noexcept;

  // Price, delta, gamma, vega, theta at (S, K, sigma).
  [[nodiscard]] RecordGreeks greeks(double S, double K, double sigma) const noexcept;

private:
  struct Jet {
    double c = 0.0, c_k = 0.0, c_kk = 0.0, c_s = 0.0;
  };
  [[nodiscard]] Jet eval_jet(double k_log, double sigma, bool want_k) const noexcept;
  [[nodiscard]] bool in_exercise_region(double k_log, double sigma) const noexcept;

  std::array<double, static_cast<std::size_t>(kNk) * kNs> coefs_{}; // [j * kNk + i]
  std::array<double, kNs> edge_{};   // unclamped k*(sigma), Chebyshev in sigma
  std::array<double, kNs> domain_{}; // continuation-domain endpoint d(sigma)
  double T_ = 0.0;
  double r_ = 0.0;
  double q_ = 0.0;
  double growth_ = 1.0; // e^{(r-q)T}
  double df_ = 1.0;     // e^{-rT}
  double k_min_ = 0.0;
  double k_max_ = 0.0;
  double s_min_ = 0.0;
  double s_max_ = 0.0;
  double cert_err_ = 0.0;
  std::uint32_t cert_pts_ = 0;
  Side side_ = Side::Put;
  bool populated_ = false;
  bool american_ = false;
  bool has_edge_ = false;
};

// Bounded, preallocated bank of records (e.g. 20 expiries x 2 sides for one
// underlier). Capacity is fixed at construction; insert() past it is refused, so
// pointers returned by at()/find() stay valid for the bank's lifetime.
class ExpiryRecordBank {
public:
  explicit ExpiryRecordBank(std::size_t capacity);

  // Append a populated record; returns its index.
  // @return OutOfRange when the bank is full; InvalidArgument for an unpopulated
  //         record.
  [[nodiscard]] Result<std::size_t> insert(const ExpiryRecord &record);

  [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  // nullptr when i >= size().
  [[nodiscard]] const ExpiryRecord *at(std::size_t i) const noexcept;
  // The record for (T, side) whose expiry equals T exactly; nullptr if none.
  [[nodiscard]] const ExpiryRecord *find(double T, Side side) const noexcept;

private:
  std::vector<ExpiryRecord> records_; // reserved to capacity_ once; never reallocates
  std::size_t capacity_ = 0;
};

} // namespace atx::vol
