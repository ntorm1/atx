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
// L_t replaces --aim-leverage in the book's config for its rule (P9 C1, DEC-10: the rule is the
// book's NavReplayConfig::leverage and its state the book's own BookLeverage, so books on a pool
// and the variants of a construction grid run it; strategy_nav_replay.cpp plans through it):
// aim-partial-v5 moves toward L_t x desired; spo-v3 tracks L_t x desired, while its gross sanity
// bound 2 x L and its one gamma calibration (on L x desired) read the run's L
// (spo::BookDecision::base_leverage), so its tracker differs from its parent's only by the aim's
// scale. The shared desired target and its ADV cap (--adv-hold-q, which reads L) are unchanged.
// Every decision the replay plans runs the scaler (the warm-up included; every book from a clean
// state of its own); the main books' scored decisions are recorded (a capacity book records none).
//
// vol-target-v1 (platform v8 Y, lane YCOMB; `--vol-target vol-target-v1`, Law::vol_target_v1)
// runs on the same scaler, store and forecast with the law of engine::book::vol_target.hpp:
// L_t = clip(L x sigma_ref_t / sigma_hat_t, 1, L), sigma_ref_t the running mean of the book's
// estimates (sigma_hat_t included), cadence 21, L before the first estimate (L = the run's
// --aim-leverage, the cap); aim-partial-v5 only. Its published text is strategy_vol_target.hpp;
// its series is <output>/vol_target.csv and its blocks are keyed "vol_target". Without the flag
// (Law::risk_target_v1, the default) every risk-target-v1 byte is unchanged.

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
#include "atx/engine/book/vol_target.hpp"
#include "strategy_nav_replay.hpp"
#include "strategy_spo.hpp"
#include "strategy_target_replay.hpp"

namespace atx::impl::strategy::risk_target {

// The scaler's law: risk-target-v1 (v8 R-8, the default) or vol-target-v1 (v8 Y).
enum class Law : atx::u8 { risk_target_v1, vol_target_v1 };
// The parsed flags: on with --risk-target S (params S, b, C: engine::book::validate_risk_target)
// or with --vol-target vol-target-v1 (law vol_target_v1; params unused: its constants are
// registered in engine::book::vol_target.hpp).
struct Options {
  bool on{};
  atx::engine::book::RiskTargetParams params{};
  Law law{Law::risk_target_v1};
};

// One scored decision of one book (a main book): the estimate in force and L_t
// (strategy_nav_replay.hpp NavLeverageRecord, which a replay book's NavReplayResult carries):
// rebalance; updated (this decision took the estimate in force); base (L, the book's
// --aim-leverage); of the estimate in force (NaN before the first) the book's gross and the share
// of it on names with a risk row, sigma_hat and S / (b sigma_hat) before the clip (raw); leverage
// (L_t in force, L before the first estimate); clip; sigma_ref (vol-target-v1 only: the running
// mean of the book's estimates at the estimate in force; NaN under risk-target-v1 and before the
// first estimate).
using Record = NavLeverageRecord;

// The NavReplayConfig::leverage rule of these options and store (Fixed when the options are off;
// P9 C1, DEC-10), and back (off for Fixed).
[[nodiscard]] NavLeverageRule leverage_rule(const Options& o,
                                            std::shared_ptr<const spo::RiskStore> risk);
[[nodiscard]] Options options_of(const NavLeverageRule& rule) noexcept;

// One book's scaler (P9 C1, DEC-10: the leverage rule's per-book state): its engine state, the
// scratch of one estimate and its store check. A replay book owns one, so books on a pool, grid
// variants and capacity books share no scaler state.
class BookScaler {
public:
  BookScaler(const Options& o, std::shared_ptr<const spo::RiskStore> risk);
  // L_t of the book at decision d. Re-estimates when due and the store forecasts d, from
  // `current` (the book's weights at its DECIDE, one per instrument) and the store's row d;
  // `base` = the book's --aim-leverage; the decision's Record (labelled `book`) is appended to
  // *records unless records is null. InvalidArgument without a store, on geometry or a base that
  // is not finite and positive; the store's axes are checked against x once (spo-v1's check).
  [[nodiscard]] atx::core::Result<atx::f64> leverage(const TargetReplayInput& x, atx::usize d,
                                                     bool rebalance, std::string_view book,
                                                     atx::f64 base,
                                                     std::span<const atx::f64> current,
                                                     std::vector<Record>* records);
  [[nodiscard]] const Options& options() const noexcept { return options_; }

private:
  [[nodiscard]] atx::core::Result<bool> estimate(atx::usize d, atx::f64 base,
                                                 std::span<const atx::f64> current);
  Options options_;
  std::shared_ptr<const spo::RiskStore> risk_;
  bool axes_checked_{};
  atx::engine::book::RiskTargetState state_; // risk-target-v1
  atx::engine::book::VolTargetState vol_;    // vol-target-v1
  atx::f64 gross_{spo::unset}, priced_share_{spo::unset}; // of the latest estimate (NaN: none)
  spo::RiskSlice slice_; // scratch of one estimate (the priced names, in instrument order)
  std::vector<atx::u32> group_;
  std::vector<atx::f64> exposures_, specific_, weights_;
};

// One book's leverage stage (P9 C1, DEC-10): its scaler and the plan inputs at L_t. The NAV
// replay's books (strategy_nav_replay.cpp) and the v7 seam's books (strategy_nav_v7.cpp) plan
// through it, so both run one rule.
class BookLeverage {
public:
  BookLeverage(const Options& o, std::shared_ptr<const spo::RiskStore> risk);
  // Decision d of the book: L_t from its scaler (BookScaler::leverage on `current`, the Record to
  // *records unless null), then config() is cfg at aim_leverage L_t and the result points at the
  // desired target the rule plans toward: `desired` itself, or under two-speed-v1 on a rebalance
  // the netted desired target plus the carry of F (`two_speed_fast`, entering the rebalance) from
  // the book's lambda = L_t / L at its previous two-speed rebalance (its first: lambda itself, no
  // carry; engine::book::two_speed_carry, Ruling PM8-16 #10). InvalidArgument before the scaler
  // moves when a two-speed rebalance lacks F. The pointer is valid until the next call.
  [[nodiscard]] atx::core::Result<const std::vector<atx::f64>*> plan(
      const TargetReplayInput& x, const NavReplayConfig& cfg, atx::usize d, bool rebalance,
      std::string_view book, std::span<const atx::f64> current,
      const std::vector<atx::f64>& desired, std::span<const atx::f64> two_speed_fast,
      std::vector<Record>* records);
  // The config of the latest plan() (cfg at L_t).
  [[nodiscard]] const NavReplayConfig& config() const noexcept { return scaled_; }
  [[nodiscard]] BookScaler& scaler() noexcept { return scaler_; }

private:
  BookScaler scaler_;
  NavReplayConfig scaled_;
  atx::f64 lambda_{spo::unset}; // two-speed-v1: lambda at the previous rebalance (NaN: none)
  std::vector<atx::f64> carried_;
};

// The scaler of the callers that plan by book label (the v7 seam v7::plan and the tests): one
// BookLeverage per label, and every Record they keep plus those a replay's books handed back
// (append), in order.
class Scaler {
public:
  Scaler(const Options& o, std::shared_ptr<const spo::RiskStore> risk);
  // A new replay pass: every book's state restarts; the records are kept.
  void begin_run();
  // L_t of `book` at decision d (BookScaler::leverage of its stage); `record` keeps the Record
  // here (the main books' scored decisions).
  [[nodiscard]] atx::core::Result<atx::f64> leverage(const TargetReplayInput& x, atx::usize d,
                                                     bool rebalance, std::string_view book,
                                                     atx::f64 base,
                                                     std::span<const atx::f64> current,
                                                     bool record);
  // The leverage stage of `book` (created on first use; begin_run drops it).
  [[nodiscard]] BookLeverage& book(std::string_view label);
  // Where a book planned through book() keeps its Records (this scaler's records()).
  [[nodiscard]] std::vector<Record>* sink() noexcept { return &records_; }
  // Records a replay's own books kept (NavReplayResult::leverage), appended in order.
  void append(std::span<const Record> records);
  [[nodiscard]] std::span<const Record> records() const noexcept { return records_; }
  [[nodiscard]] const Options& options() const noexcept { return options_; }

private:
  Options options_;
  std::shared_ptr<const spo::RiskStore> risk_;
  std::map<std::string, BookLeverage, std::less<>> books_;
  std::vector<Record> records_;
};

// risk_target.csv: one line per record, columns
//   session,book,rebalance,updated,gross,priced_share,sigma_hat,raw,L_t,multiplier,clip
// (multiplier = L_t / L; clip -1 at .8 L, 1 at 1.25 L, 0 inside; nan before the first estimate).
// Law vol_target_v1 (vol_target.csv): sigma_ref after sigma_hat; clip -1 at the floor 1, 1 at L.
[[nodiscard]] std::string records_csv(std::span<const Record> records,
                                      Law law = Law::risk_target_v1);
// The rule's text and its parameter block (recipe.json, summary.json, holdings manifest,
// v7_extras.json key "risk_target").
[[nodiscard]] std::string declaration(const Options& o);
[[nodiscard]] nlohmann::json parameters_json(const Options& o);
// Per book over the records: decisions, estimates, L, L_t and the multiplier (n, mean, min,
// max), decisions and estimates at each clip, decisions before the first estimate, sigma_hat
// over the estimates. Law vol_target_v1 adds sigma_ref over the estimates.
[[nodiscard]] nlohmann::json summary_json(std::span<const Record> records,
                                          Law law = Law::risk_target_v1);
// The rule id suffix: "+risk-target-S", then "-bias-b" / "-cadence-C" off the registered values;
// "+vol-target-v1" under the vol target.
[[nodiscard]] std::string rule_suffix(const Options& o);
// The published names of the law: the block key of recipe.json / summary.json / holdings
// manifest / v7_extras.json ("risk_target" | "vol_target") and the series file
// ("risk_target.csv" | "vol_target.csv").
[[nodiscard]] const char* block_key(const Options& o) noexcept;
[[nodiscard]] const char* series_file(const Options& o) noexcept;
// The leverage rows of `nav --list-rules --json` (K-P9-7, Ruling P7): fixed-v1, vol-target-v1 and
// risk-target-v1, each {id, kind "leverage", params_schema (a JSON Schema object whose properties
// name their nav flag), incompatible (rule ids)}.
[[nodiscard]] nlohmann::json leverage_rules_json();
} // namespace atx::impl::strategy::risk_target
