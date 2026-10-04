// research/admission (P9 B1): the PM7-35 sign rule as one predicate. Both Python spellings are
// written as the two values of SignRule; the gate count and the wave decision read the same
// predicate, so they cannot disagree on a runner sign of 0 again (composition review section 4).
#include <gtest/gtest.h>

#include <array>

#include "atx/engine/research/admission/sign_rule.hpp"

namespace adm = atx::engine::research::admission;
using adm::CandidateKind;
using adm::SignRule;
using adm::SignVerdict;

// Compile-time: the predicate is constexpr and the ruled default is today's wave rule (Ruling P9).
static_assert(adm::admitted_sign_stands(SignRule::GatePriorV1, 1, 1));
static_assert(!adm::admitted_sign_stands(SignRule::GatePriorV1, 1, 0));
static_assert(adm::admitted_sign_stands(SignRule::WaveZeroKeptV1, 1, 0));
static_assert(adm::kPm735SignRule == SignRule::WaveZeroKeptV1);

// The gate (research_cycle.py:1420, sign_agrees): an admitted string counts only with its runner
// sign equal to its prior; runner sign 0 or the opposite sign does not count; not admitted never
// counts.
TEST(ResearchAdmission, SignPredicateGateRule) {
  EXPECT_TRUE(adm::admitted_sign_stands(SignRule::GatePriorV1, 1, 1));
  EXPECT_FALSE(adm::admitted_sign_stands(SignRule::GatePriorV1, 1, 0));
  EXPECT_FALSE(adm::admitted_sign_stands(SignRule::GatePriorV1, 1, -1));
  EXPECT_TRUE(adm::gate_counts(SignRule::GatePriorV1, true, 1, 1));
  EXPECT_FALSE(adm::gate_counts(SignRule::GatePriorV1, true, 1, 0));
  EXPECT_FALSE(adm::gate_counts(SignRule::GatePriorV1, false, 1, 1));
  // Under the gate rule the wave drops an admitted addition with runner sign 0: no disagreement.
  EXPECT_EQ(adm::pm7_35(SignRule::GatePriorV1, CandidateKind::Addition, true, 1, 0),
            SignVerdict::Drop);
}

// The wave (wave_rules.py:50-51): runner sign 0 stands next to an admitted string (R-2
// precedent); the opposite sign does not. The gate under the same rule counts it too.
TEST(ResearchAdmission, SignPredicateWaveRule) {
  EXPECT_TRUE(adm::admitted_sign_stands(SignRule::WaveZeroKeptV1, 1, 1));
  EXPECT_TRUE(adm::admitted_sign_stands(SignRule::WaveZeroKeptV1, 1, 0));
  EXPECT_FALSE(adm::admitted_sign_stands(SignRule::WaveZeroKeptV1, 1, -1));
  EXPECT_TRUE(adm::gate_counts(SignRule::WaveZeroKeptV1, true, 1, 0));
  EXPECT_FALSE(adm::gate_counts(SignRule::WaveZeroKeptV1, false, 1, 0));
}

// pm7_35 under the wave rule reproduces wave_rules.sign_pm7_35's decision table (prior +1):
// additions -- not admitted: keep (weight 0); admitted with runner = prior or 0: keep; admitted
// against the prior: drop. Replacements -- keep only admitted with runner = prior, under every
// rule.
TEST(ResearchAdmission, SignPredicatePm735DecisionTable) {
  struct Case {
    CandidateKind kind;
    bool admitted;
    atx::i32 runner;
    SignVerdict wave;
    SignVerdict gate;
  };
  constexpr std::array<Case, 12> table{{
      {CandidateKind::Addition, false, 1, SignVerdict::Keep, SignVerdict::Keep},
      {CandidateKind::Addition, false, 0, SignVerdict::Keep, SignVerdict::Keep},
      {CandidateKind::Addition, false, -1, SignVerdict::Keep, SignVerdict::Keep},
      {CandidateKind::Addition, true, 1, SignVerdict::Keep, SignVerdict::Keep},
      {CandidateKind::Addition, true, 0, SignVerdict::Keep, SignVerdict::Drop},
      {CandidateKind::Addition, true, -1, SignVerdict::Drop, SignVerdict::Drop},
      {CandidateKind::Replacement, false, 1, SignVerdict::Drop, SignVerdict::Drop},
      {CandidateKind::Replacement, false, 0, SignVerdict::Drop, SignVerdict::Drop},
      {CandidateKind::Replacement, false, -1, SignVerdict::Drop, SignVerdict::Drop},
      {CandidateKind::Replacement, true, 1, SignVerdict::Keep, SignVerdict::Keep},
      {CandidateKind::Replacement, true, 0, SignVerdict::Drop, SignVerdict::Drop},
      {CandidateKind::Replacement, true, -1, SignVerdict::Drop, SignVerdict::Drop},
  }};
  for (const Case &c : table) {
    EXPECT_EQ(adm::pm7_35(SignRule::WaveZeroKeptV1, c.kind, c.admitted, 1, c.runner), c.wave)
        << static_cast<int>(c.kind) << ' ' << c.admitted << ' ' << c.runner;
    EXPECT_EQ(adm::pm7_35(SignRule::GatePriorV1, c.kind, c.admitted, 1, c.runner), c.gate)
        << static_cast<int>(c.kind) << ' ' << c.admitted << ' ' << c.runner;
  }
}

// An unknown rule fails closed (nothing stands, every candidate is dropped); names round-trip.
TEST(ResearchAdmission, SignPredicateUnknownFailsClosedAndNamesRoundTrip) {
  EXPECT_FALSE(adm::admitted_sign_stands(SignRule::Unknown, 1, 1));
  EXPECT_FALSE(adm::gate_counts(SignRule::Unknown, true, 1, 1));
  EXPECT_EQ(adm::pm7_35(SignRule::Unknown, CandidateKind::Addition, false, 1, 1),
            SignVerdict::Drop);
  EXPECT_EQ(adm::sign_rule_name(SignRule::GatePriorV1), "gate-prior-v1");
  EXPECT_EQ(adm::sign_rule_name(SignRule::WaveZeroKeptV1), "wave-zero-kept-v1");
  EXPECT_EQ(adm::sign_rule_name(SignRule::Unknown), "");
  EXPECT_EQ(adm::sign_rule_from_name("gate-prior-v1"), SignRule::GatePriorV1);
  EXPECT_EQ(adm::sign_rule_from_name("wave-zero-kept-v1"), SignRule::WaveZeroKeptV1);
  EXPECT_FALSE(adm::sign_rule_from_name("pm7-35").has_value());
}
