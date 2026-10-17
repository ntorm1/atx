#pragma once

// mv-aim-v1 (P9 lane COV-MV, cov-design section 5.2, Rulings COV-7, W3-17, PRE-24a, COV-13 (c)):
// the NAV path's target rule (K-P9-7 kind target, `--target-rule mv-aim-v1`, row in
// book/nav_rules.cpp), reached through the v7 seam (strategy_nav_v7). At each rebalance decision d
// whose shared desired target proceeds, on the calling thread, after the target's construction
// (book/nav_decide.cpp form_desired_target) and before any book's construction rule:
//
//   members M_d     the role's members at d; a NaN desired weight is no opinion (untouched)
//   priced P_d      members with a finite desired weight and a risk row at d: industry slot
//                   0..49 and a finite specific variance D_i > 0 (book_variance's priced names)
//   desired_P  <-   g x w,  w = engine::book::mv_aim(row d; score desired_i, sigma_i = sqrt(D_i))
//                   = P V_d^-1 P alpha / sum |.|,  alpha_i = sigma_i desired_i,
//                   V_d = X F X' + D of row d (F never inverted), g = sum_{i in P_d} |desired_i|
//
// so the priced block keeps the desired target's gross over P_d (Ruling W3-17) and is dollar
// neutral; unpriced members and nonmembers keep their weights, so the vector's total gross is the
// desired target's. Row d is read with RiskStore::read(d), the index BookScaler::estimate and spo
// read (Ruling COV-4): its stamp is as_of = cutoff = d (K-P9-12), i.e. nothing after the
// decision's own session. Pass-through (desired unchanged, counted, never silent): no forecast at
// d, fewer than two priced names, or a flat tilt. Refused (InvalidArgument, the run stops): no
// store, the store's axes not the replay's (checked at the run's first decision, Ruling COVMV-S
// S-6), an industry slot of row d outside 0..49 and 255 (Ruling COV-11, S11: the container reader
// does not check it), a kernel refusal.
//
// No constant: no IC, horizon, ridge, cap, clip or floor (the positive scalars wash out under
// the gross normalisation); the store is the cell's pin. Under --locate-in-aim the target can be
// negative on a special-tier name (the books' locate block still applies at the plan; counted).
// Determinism: fixed loop order, one call per decision on the calling thread (books on a pool
// and the capacity books all read the one shared target).
//
// Printed only (summary.json `mv_aim`, main pass; never in the target's arithmetic): decisions
// (formed, applied, each pass-through, warm-up), priced / unpriced name counts, the specific-vol
// ratio of Ruling PRE-24a (sum |w| sigma / sum |desired| sigma over P_d, pooled and per
// decision), the share of the target's forecast factor variance on eigen ranks 00-02 of F_d
// (Ruling COV-13 (c); rank 00 the smallest, COV's convention) for the MV and the desired target,
// and the special-tier negative count. A decision is counted once: the first time its index is
// formed in the run (a --calibrate-gross pass that forms the construction again counts nothing).
// A print never stops a decision (Ruling COVMV-S S-2): when the eigen share fails at a decision
// (F_d's forecast block not symmetric, no convergence) the decision stands as formed, the
// failure is counted with the first error, and the summary's eigen_00_02_share is null. The
// print's storage is kept across decisions in the Rule (FactorEigen; Ruling COVMV-S S-1).

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json_fwd.hpp>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/book/mv_target.hpp"
#include "strategy_spo.hpp"
#include "strategy_target_replay.hpp"

namespace atx::impl::strategy::mv_aim {

inline constexpr std::string_view rule_id = "mv-aim-v1";
// The eigen ranks the print reads (00-02, Ruling COV-13 (c)).
inline constexpr atx::usize low_ranks = 3;

// The rule's published sentence (recipe.json, summary.json, the holdings manifest).
[[nodiscard]] std::string declaration();

// Everything the rule holds at `names` instruments (the store's row, the priced view, the
// kernel's scratch, the eigen print), saturating: the NAV workspace reserve's term
// (nav_workspace_reserve_bytes through v7::extension_workspace_bytes, Ruling COVMV-1).
[[nodiscard]] atx::u64 workspace_bytes(atx::usize names) noexcept;

// n, mean, min and max of the finite values added.
struct Stats {
  atx::f64 sum{}, lo{}, hi{};
  atx::usize n{};
  void add(atx::f64 v) noexcept;
};

// The run's counts (the summary block reads them).
struct Counts {
  atx::usize decisions{}, applied{}, no_forecast{}, fewer_than_two_priced{}, flat{};
  atx::usize warm_up{}, warm_up_applied{};
  Stats priced, unpriced, ratio, share_mv, share_desired;
  atx::f64 pooled_target{}, pooled_desired{}; // sum |w| sigma, sum |desired| sigma (applied)
  atx::usize negative_names{}, negative_decisions{};
  bool locate_in_aim{};
  // Applied decisions whose eigen share failed (print only, Ruling COVMV-S S-2), the first error.
  atx::usize share_unavailable{};
  std::string share_error;
};

class Rule {
public:
  explicit Rule(std::shared_ptr<const spo::RiskStore> store);
  // A new run (ScopedNavExtension::begin_run): the counts start again, and the store's axes are
  // checked again at the run's first decision.
  void begin_run();
  // Decision d of `x` (the shared desired target that proceeds, one entry per instrument;
  // no_short: the locate-in-aim mask, empty without it). Rewrites the priced members' weights as
  // the header states, or leaves `desired` untouched (pass-through); counts d the first time its
  // index is formed. InvalidArgument as the header states.
  [[nodiscard]] atx::core::Status apply(const TargetReplayInput& x, atx::usize d,
                                        std::span<const atx::u8> no_short,
                                        std::vector<atx::f64>& desired);
  [[nodiscard]] const Counts& counts() const noexcept { return counts_; }
  // {rule, declaration, risk_model {directory, manifest_sha256}}: recipe.json, the holdings
  // manifest and the capacity directory's summary.
  [[nodiscard]] nlohmann::json parameters_json() const;
  // parameters_json() plus the counts (summary.json of the main pass).
  [[nodiscard]] nlohmann::json summary_json() const;

private:
  // How decision d counts: the first time the run forms its index, and scored (d >=
  // decision_begin).
  struct Visit {
    bool fresh{}, scored{};
  };
  // apply's three jobs, in order. The decision: its geometry, the store's axes once per run
  // (Ruling COVMV-S S-6; a Debug build re-checks every decision), then how it counts.
  [[nodiscard]] atx::core::Status check_input(const TargetReplayInput& x, atx::usize d,
                                              std::span<const atx::u8> no_short,
                                              const std::vector<atx::f64>& desired);
  [[nodiscard]] Visit begin_decision(const TargetReplayInput& x, atx::usize d,
                                     std::span<const atx::u8> no_short) noexcept;
  // The priced view of row d (its slots checked first, Ruling COV-11 S11); returns the unpriced
  // members.
  [[nodiscard]] atx::core::Result<atx::usize> priced_view(const TargetReplayInput& x, atx::usize d,
                                                          const std::vector<atx::f64>& desired);
  // The kernel on the view, then the priced block of `desired` at its gross g (returned; Ruling
  // W3-17). Unavailable: a flat tilt (`desired` untouched).
  [[nodiscard]] atx::core::Result<atx::f64> tilt(std::vector<atx::f64>& desired);
  // The counts of a pass-through / an applied decision; count_applied is true when the decision's
  // print is recorded (fresh and scored).
  void pass_through(Visit v, atx::usize Counts::*outcome) noexcept;
  [[nodiscard]] bool count_applied(Visit v, atx::usize unpriced) noexcept;
  // The print of one applied decision. record: the specific-vol ratio and the special-tier
  // negatives (`desired` is the rewritten target). add_eigen_shares: the eigen shares of F_d,
  // both or neither; its error is the print's, never the decision's (Ruling COVMV-S S-2), and
  // note_unavailable counts it.
  void record(std::span<const atx::u8> no_short, const std::vector<atx::f64>& desired,
              atx::f64 gross);
  [[nodiscard]] atx::core::Status add_eigen_shares();
  void note_unavailable(const atx::core::Error& error);

  std::shared_ptr<const spo::RiskStore> store_;
  bool axes_checked_{};    // the store's axes are this run's input's (Ruling COVMV-S S-6)
  bool formed_{};          // some decision index was formed in this pass
  atx::usize last_{};      // the latest index formed
  Counts counts_;
  spo::RiskSlice slice_;
  // The priced view of row d (instrument order) and its target.
  std::vector<atx::u32> group_;
  std::vector<atx::f64> exposures_, specific_, score_, sigma_, out_;
  std::vector<atx::usize> index_;
  atx::engine::book::MvAimScratch scratch_;
  atx::engine::book::FactorEigen eigen_; // the eigen print's storage, kept across decisions
  std::vector<atx::f64> weights_, exposure_;
};

} // namespace atx::impl::strategy::mv_aim
