#pragma once

// atx::engine::research::admission -- the PM7-35 sign rule as one predicate (platform P9 B1;
// composition review section 4 "Sign rule").
//
// Two Python predicates spell one ruling today and disagree on an admitted string whose runner
// (TRAIN IC) sign is 0:
//   * the gate (research_cycle.py:1420, gate.sign_agrees): an admitted string counts only when
//     its runner sign equals its prior sign (admission.json sign_agrees);
//   * the wave (wave_rules.py:50-51, sign_pm7_35): an admitted addition with runner sign 0 is
//     kept (R-2 precedent), so a wave whose only admitted string has runner sign 0 yields no cell.
// Here both are one predicate, admitted_sign_stands, parameterised by the ruled SignRule, and both
// consumers (gate_counts, pm7_35) read it, so they cannot disagree again. The PM rules which rule
// is the ruling (plan section 2.4 B1); kPm735SignRule is that single switch.

#include <initializer_list>
#include <optional>
#include <string_view>

#include "atx/core/types.hpp"

namespace atx::engine::research::admission {

// Frozen integer values (recorded in the admission manifest).
enum class SignRule : u8 {
  Unknown = 0,       // never a working value: every predicate fails closed
  GatePriorV1 = 1,   // runner sign == prior sign, as the gate requires
  WaveZeroKeptV1 = 2 // runner sign == prior sign, or runner sign 0 (wave_rules.py:50-51)
};

// Ruling P9 (wave 1, progress.md P9): the Python wave keeps today's rule (runner sign 0 kept);
// the C++ predicate defaults to the same rule until the PM rules otherwise before merge.
inline constexpr SignRule kPm735SignRule = SignRule::WaveZeroKeptV1;

// "gate-prior-v1", "wave-zero-kept-v1"; "" for Unknown.
[[nodiscard]] constexpr std::string_view sign_rule_name(SignRule rule) noexcept {
  switch (rule) {
  case SignRule::GatePriorV1:
    return "gate-prior-v1";
  case SignRule::WaveZeroKeptV1:
    return "wave-zero-kept-v1";
  case SignRule::Unknown:
    return "";
  }
  return "";
}

// The rule named `name`; nullopt when unknown.
[[nodiscard]] constexpr std::optional<SignRule>
sign_rule_from_name(std::string_view name) noexcept {
  for (const SignRule rule : {SignRule::GatePriorV1, SignRule::WaveZeroKeptV1}) {
    if (name == sign_rule_name(rule)) {
      return rule;
    }
  }
  return std::nullopt;
}

// THE predicate: does an admitted string's runner sign stand next to its prior sign under `rule`?
[[nodiscard]] constexpr bool admitted_sign_stands(SignRule rule, i32 prior_sign,
                                                  i32 runner_sign) noexcept {
  switch (rule) {
  case SignRule::GatePriorV1:
    return runner_sign == prior_sign;
  case SignRule::WaveZeroKeptV1:
    return runner_sign == prior_sign || runner_sign == 0;
  case SignRule::Unknown:
    return false;
  }
  return false;
}

// The gate's count of one listed string: admitted and its sign stands.
[[nodiscard]] constexpr bool gate_counts(SignRule rule, bool admitted, i32 prior_sign,
                                         i32 runner_sign) noexcept {
  return admitted && admitted_sign_stands(rule, prior_sign, runner_sign);
}

enum class CandidateKind : u8 {
  Addition = 1,    // a new string of the wave
  Replacement = 2, // a refinement or re-screen (add-alpha --replaces)
};

enum class SignVerdict : u8 { Keep = 1, Drop = 2 };

// PM7-35 on one screened candidate (wave_rules.sign_pm7_35's decisions under WaveZeroKeptV1):
//   replacement: kept only when admitted with runner sign == prior (under every rule: the
//                replaced member keeps its string otherwise);
//   addition:    not admitted -> kept (weight 0 in the fit, R-2 / R-7 precedent); admitted ->
//                kept iff admitted_sign_stands.
// Unknown rule: Drop.
[[nodiscard]] constexpr SignVerdict pm7_35(SignRule rule, CandidateKind kind, bool admitted,
                                           i32 prior_sign, i32 runner_sign) noexcept {
  if (rule == SignRule::Unknown) {
    return SignVerdict::Drop;
  }
  if (kind == CandidateKind::Replacement) {
    return admitted && runner_sign == prior_sign ? SignVerdict::Keep : SignVerdict::Drop;
  }
  if (!admitted) {
    return SignVerdict::Keep;
  }
  return admitted_sign_stands(rule, prior_sign, runner_sign) ? SignVerdict::Keep
                                                             : SignVerdict::Drop;
}

} // namespace atx::engine::research::admission
