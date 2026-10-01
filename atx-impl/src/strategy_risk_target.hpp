#pragma once

// risk-target-v1 (platform v8 R-8, Ruling E-40): the ex-ante risk target of the NAV replay, a
// construction option reached through strategy_nav_v7 (`--risk-target S`). Per decision d and
// book, before the construction rule plans (engine::book::risk_target.hpp):
//
//   L_t = clip(S / (b x sigma_hat_t), .8 L, 1.25 L)
//
// sigma_hat_t = sqrt(252 x book_variance) of the book's current weights (what its DECIDE reads,
// after the session's fills) scaled to gross 1 over the whole book, on the atx-risk-v1 row d of
// the pinned risk store (--risk-model / --risk-model-sha256, the spo rules' store and check),
// over the names with a risk row (an industry slot and a finite positive specific variance, as
// book_variance prices them; no specific ceiling). Estimated at the book's first estimable
// decision (a forecast at d, a book that is not flat, a positive variance) and then at the first
// one at least C sessions after its previous estimate; held between estimates; L before the
// first. S = --risk-target, b = --risk-target-bias (registered 1.15), C =
// --risk-target-cadence (registered 21); the clip [.8, 1.25] and the annualisation 252 are
// registered constants. L = the book's --aim-leverage.
//
// L_t replaces --aim-leverage in the book's config for its rule (strategy_nav_v7.cpp):
// aim-partial-v5 moves toward L_t x desired; spo-v3 tracks L_t x desired, while its gross sanity
// bound 2 x L and its one gamma calibration (on L x desired) read the run's L
// (spo::BookDecision::base_leverage), so its tracker differs from its parent's only by the aim's
// scale. The shared desired target and its ADV cap (--adv-hold-q, which reads L) are unchanged.
// Every decision the replay plans runs the scaler (the warm-up included; each pass from a clean
// state, every book on its own state); the main pass's scored decisions are recorded.

#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json_fwd.hpp>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/book/risk_target.hpp"
#include "strategy_spo.hpp"
#include "strategy_target_replay.hpp"

namespace atx::impl::strategy::risk_target {

// The parsed flags: on with --risk-target S; params S, b, C (engine::book::validate_risk_target).
struct Options {
  bool on{};
  atx::engine::book::RiskTargetParams params{};
};

// One scored decision of one book (main pass): the estimate in force and L_t.
struct Record {
  atx::i64 session{};
  std::string book;
  bool rebalance{}, updated{}; // updated: this decision took the estimate in force
  atx::f64 base{};             // L, the book's --aim-leverage
  // Of the estimate in force (NaN before the first): the book's gross and the share of it on
  // names with a risk row, sigma_hat and S / (b sigma_hat) before the clip.
  atx::f64 gross{}, priced_share{}, sigma_hat{}, raw{};
  atx::f64 leverage{}; // L_t in force (L before the first estimate)
  atx::engine::book::RiskTargetClip clip{atx::engine::book::RiskTargetClip::None};
};

// The per-run scaler: the risk store, one engine state per book, the records.
class Scaler {
public:
  Scaler(const Options& o, std::shared_ptr<const spo::RiskStore> risk);
  // A new replay pass: every book's state restarts; the records are kept.
  void begin_run();
  // L_t of `book` at decision d. Re-estimates when due and the store forecasts d, from
  // `current` (the book's weights at its DECIDE, one per instrument) and the store's row d;
  // `base` = the book's --aim-leverage; `record` keeps a Record (the main pass's scored
  // decisions). InvalidArgument without a store, on geometry or a base that is not finite and
  // positive; the store's axes are checked against x once (spo-v1's check).
  [[nodiscard]] atx::core::Result<atx::f64> leverage(const TargetReplayInput& x, atx::usize d,
                                                     bool rebalance, std::string_view book,
                                                     atx::f64 base,
                                                     std::span<const atx::f64> current,
                                                     bool record);
  [[nodiscard]] std::span<const Record> records() const noexcept { return records_; }
  [[nodiscard]] const Options& options() const noexcept { return options_; }

private:
  struct Book {
    atx::engine::book::RiskTargetState state;
    atx::f64 gross{spo::unset}, priced_share{spo::unset}; // of the latest estimate (NaN: none)
  };
  [[nodiscard]] atx::core::Result<bool> estimate(atx::usize d, atx::f64 base,
                                                 std::span<const atx::f64> current, Book& book);
  Options options_;
  std::shared_ptr<const spo::RiskStore> risk_;
  bool axes_checked_{};
  std::map<std::string, Book, std::less<>> books_;
  std::vector<Record> records_;
  spo::RiskSlice slice_; // scratch of one estimate (the priced names, in instrument order)
  std::vector<atx::u32> group_;
  std::vector<atx::f64> exposures_, specific_, weights_;
};

// risk_target.csv: one line per record, columns
//   session,book,rebalance,updated,gross,priced_share,sigma_hat,raw,L_t,multiplier,clip
// (multiplier = L_t / L; clip -1 at .8 L, 1 at 1.25 L, 0 inside; nan before the first estimate).
[[nodiscard]] std::string records_csv(std::span<const Record> records);
// The rule's text and its parameter block (recipe.json, summary.json, holdings manifest,
// v7_extras.json key "risk_target").
[[nodiscard]] std::string declaration(const Options& o);
[[nodiscard]] nlohmann::json parameters_json(const Options& o);
// Per book over the records: decisions, estimates, L, L_t and the multiplier (n, mean, min,
// max), decisions and estimates at each clip, decisions before the first estimate, sigma_hat
// over the estimates.
[[nodiscard]] nlohmann::json summary_json(std::span<const Record> records);
// The rule id suffix: "+risk-target-S", then "-bias-b" / "-cadence-C" off the registered values.
[[nodiscard]] std::string rule_suffix(const Options& o);
} // namespace atx::impl::strategy::risk_target
