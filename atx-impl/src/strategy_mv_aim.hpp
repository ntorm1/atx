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
// store, the store's axes not the replay's, an industry slot of row d outside 0..49 and 255
// (Ruling COV-11, S11: the container reader does not check it), a kernel refusal.
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
};

class Rule {
public:
  explicit Rule(std::shared_ptr<const spo::RiskStore> store);
  // A new pass of the run (begin_run): the counts and the axes check start again.
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
  // The print of one applied decision (eigen share of F_d, the specific-vol ratio, the
  // special-tier negatives); `desired` is the rewritten target.
  [[nodiscard]] atx::core::Status record(std::span<const atx::u8> no_short,
                                         const std::vector<atx::f64>& desired, atx::f64 gross);
  std::shared_ptr<const spo::RiskStore> store_;
  const atx::i64* axes_{}; // the session keys check_axes accepted (per input)
  bool formed_{};          // some decision index was formed in this pass
  atx::usize last_{};      // the latest index formed
  Counts counts_;
  spo::RiskSlice slice_;
  // The priced view of row d (instrument order) and its target.
  std::vector<atx::u32> group_;
  std::vector<atx::f64> exposures_, specific_, score_, sigma_, out_;
  std::vector<atx::usize> index_;
  atx::engine::book::MvAimScratch scratch_;
  atx::engine::book::FactorEigen eigen_;
  std::vector<atx::f64> weights_, exposure_;
};

} // namespace atx::impl::strategy::mv_aim
