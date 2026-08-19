// Gate for the alpha layer: the registry, the name-resolved panel schema, and
// the input/target adjudicator.
//
// The adjudicator cases are not synthetic. Each one reproduces a contamination
// this repo has already shipped and documented in PROSE, and asserts that the
// declared-`reads` machinery finds it without a human reading source:
//
//   * `label_contaminated` carrying the entry IV mark (rounds 1-4 headline).
//   * `f12_bf25_21d` / `f14_iv_chg_5d` carrying it against an IV-change axis
//     (round 9's "must not be read at lag 0" comment).
//   * `--feature-lag 2` closing that channel (round 9's assertion, here a
//     computed fact).
//   * `vol_chg_21d`'s trailing-vol leg, which its own docstring in
//     `tools/vrp_train.hpp` describes as "no trivial trailing-vol persistence".
//
// And the cases that must stay CLEAN, because an adjudicator that flags
// everything is worth nothing: Goyal-Saretto against the realized-vol axis is
// a legal, uncontaminated predictor and has to come back silent.

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/alpha/audit.hpp"
#include "atx/vol/alpha/frame.hpp"
#include "atx/vol/alpha/registry.hpp"
#include "atx/vol/alpha/schema.hpp"
#include "atx/vol/alpha/spec.hpp"

namespace {

using atx::vol::alpha::AuditConfig;
using atx::vol::alpha::AuditReport;
using atx::vol::alpha::builtin_features;
using atx::vol::alpha::builtin_targets;
using atx::vol::alpha::FeatureRegistry;
using atx::vol::alpha::FeatureSpec;
using atx::vol::alpha::FindingKind;
using atx::vol::alpha::PanelFrame;
using atx::vol::alpha::glob_match;
using atx::vol::alpha::PanelSchema;
using atx::vol::alpha::Severity;
using atx::vol::alpha::SignPrior;
using atx::vol::alpha::spot;
using atx::vol::alpha::TargetSpec;
using atx::vol::alpha::Unit;
using atx::vol::alpha::Window;

// Resolve one catalogue feature by name, or fail the test loudly.
const FeatureSpec *feat(const FeatureRegistry &reg, std::string_view name) {
  const FeatureSpec *spec = reg.find(name);
  EXPECT_NE(spec, nullptr) << "catalogue is missing '" << name << "'";
  return spec;
}

const TargetSpec &targ(const atx::vol::alpha::TargetRegistry &reg, std::string_view name) {
  const TargetSpec *spec = reg.find(name);
  EXPECT_NE(spec, nullptr) << "catalogue is missing target '" << name << "'";
  return *spec;
}

AuditReport run_audit(std::vector<const FeatureSpec *> features, const TargetSpec &target,
                      std::ptrdiff_t lag = 0, bool headline = false) {
  AuditConfig cfg;
  cfg.feature_lag = lag;
  cfg.target_is_headline = headline;
  return atx::vol::alpha::audit(features, target, cfg);
}

// ── Window arithmetic ───────────────────────────────────────────────────────

TEST(AlphaWindow, CausalityIsTheSingleInequality) {
  EXPECT_TRUE((Window{-20, 0}).causal());
  EXPECT_TRUE((Window{-1, -1}).causal());
  EXPECT_FALSE((Window{1, 21}).causal());
  EXPECT_FALSE((Window{0, 1}).causal());
}

TEST(AlphaWindow, WarmupCountsOnlyTrailingSessions) {
  EXPECT_EQ((Window{-20, 0}).warmup(), 20U);
  EXPECT_EQ((Window{0, 0}).warmup(), 0U);
  EXPECT_EQ((Window{1, 21}).warmup(), 0U);
}

TEST(AlphaWindow, LagShiftsBothEndpointsIntoThePast) {
  const Window w{-5, 0};
  const Window l = w.lagged(2);
  EXPECT_EQ(l.first, -7);
  EXPECT_EQ(l.last, -2);
  // A trailing window and its lagged self still overlap; the entry SESSION is
  // what a lag removes, and that is exactly what closes an entry-mark channel.
  EXPECT_TRUE(w.overlaps(l));
  EXPECT_FALSE((Window{0, 0}).overlaps(l));
}

// ── Glob selection ──────────────────────────────────────────────────────────

TEST(AlphaGlob, MatchesRunsAndSingleCharacters) {
  EXPECT_TRUE(glob_match("*", "anything"));
  EXPECT_TRUE(glob_match("f4_term_slope", "f4_term_slope"));
  EXPECT_TRUE(glob_match("f1?_*", "f16_iv_vov_21d"));
  EXPECT_TRUE(glob_match("liq_*", "liq_hspread_frac"));
  EXPECT_TRUE(glob_match("*_slope_*", "f17_slope_126d"));
  EXPECT_FALSE(glob_match("f1?_*", "f4_term_slope"));
  EXPECT_FALSE(glob_match("liq_*", "f0_log_rv1"));
  // A pattern that is a strict prefix must not match: `--features f4_term_slop`
  // is a typo, not a selector.
  EXPECT_FALSE(glob_match("f4_term_slop", "f4_term_slope"));
}

// ── Registry ────────────────────────────────────────────────────────────────

TEST(AlphaRegistry, RejectsDuplicateAndEmptyNames) {
  FeatureRegistry reg;
  ASSERT_TRUE(reg.add(FeatureSpec{"a", Unit::Fraction, SignPrior::None, "", {spot(-1, 0)}, ""}));
  EXPECT_FALSE(reg.add(FeatureSpec{"a", Unit::Fraction, SignPrior::None, "", {spot(-1, 0)}, ""}));
  EXPECT_FALSE(reg.add(FeatureSpec{"", Unit::Fraction, SignPrior::None, "", {spot(-1, 0)}, ""}));
  EXPECT_EQ(reg.size(), 1U);
}

TEST(AlphaRegistry, SelectReturnsCatalogueOrderNotPatternOrder) {
  const FeatureRegistry reg = builtin_features();
  const std::vector<std::string> patterns{"f5_hv_iv_gap", "f0_log_rv1"};
  const auto sel = reg.select(patterns);
  ASSERT_TRUE(sel) << sel.error().message();
  ASSERT_EQ(sel->size(), 2U);
  EXPECT_EQ((*sel)[0]->name, "f0_log_rv1");
  EXPECT_EQ((*sel)[1]->name, "f5_hv_iv_gap");
}

TEST(AlphaRegistry, SelectRefusesAPatternThatMatchesNothing) {
  const FeatureRegistry reg = builtin_features();
  const std::vector<std::string> patterns{"f4_term_slop"};
  const auto sel = reg.select(patterns);
  ASSERT_FALSE(sel);
  EXPECT_EQ(sel.error().code(), atx::core::ErrorCode::NotFound);
}

TEST(AlphaRegistry, SelectDeduplicatesOverlappingPatterns) {
  const FeatureRegistry reg = builtin_features();
  const std::vector<std::string> patterns{"f1?_*", "f16_iv_vov_21d"};
  const auto sel = reg.select(patterns);
  ASSERT_TRUE(sel) << sel.error().message();
  const auto hits = std::count_if(sel->begin(), sel->end(), [](const FeatureSpec *s) {
    return s->name == "f16_iv_vov_21d";
  });
  EXPECT_EQ(hits, 1);
}

TEST(AlphaRegistry, EveryCataloguedFeatureIsCausalAndWellFormed) {
  const FeatureRegistry reg = builtin_features();
  ASSERT_FALSE(reg.empty());
  for (const FeatureSpec &spec : reg.all()) {
    EXPECT_TRUE(spec.causal()) << spec.name << " declares a forward read";
    EXPECT_FALSE(spec.reads.empty()) << spec.name << " declares no inputs";
    EXPECT_FALSE(spec.doc.empty()) << spec.name << " has no docstring";
    for (const auto &ref : spec.reads) {
      EXPECT_TRUE(ref.window.well_formed()) << spec.name;
    }
  }
}

TEST(AlphaRegistry, ASignPriorRequiresACitation) {
  // A prior is a claim about the published literature. An unsourced feature
  // may not carry one -- that is how a fitting choice gets laundered into a
  // "predicted sign" the measurement is then graded against.
  const FeatureRegistry reg = builtin_features();
  for (const FeatureSpec &spec : reg.all()) {
    if (spec.prior != SignPrior::None) {
      EXPECT_FALSE(spec.citation.empty())
          << spec.name << " claims prior '" << to_string(spec.prior) << "' with no citation";
    }
  }
}

TEST(AlphaRegistry, ExactlyTheKnownTradeableTargetsAreMarkedTradeable) {
  const auto reg = builtin_targets();
  EXPECT_FALSE(targ(reg, "rv_fwd_21d").tradeable);
  EXPECT_FALSE(targ(reg, "vol_chg_21d").tradeable);
  EXPECT_FALSE(targ(reg, "label_contaminated").tradeable);
  // A constant-maturity index difference is not a position: the 21d option
  // bought at t has expired by t+H.
  EXPECT_FALSE(targ(reg, "iv_chg_21d_raw").tradeable);
  EXPECT_TRUE(targ(reg, "iv_chg_21d_roll").tradeable);
  EXPECT_TRUE(targ(reg, "dh_straddle_pnl_21d").tradeable);
}

// ── Schema ──────────────────────────────────────────────────────────────────

TEST(AlphaSchema, HeaderRoundTrips) {
  const auto schema = PanelSchema::from_header("symbol\tdate\tf0_log_rv1\tlabel");
  ASSERT_TRUE(schema) << schema.error().message();
  EXPECT_EQ(schema->size(), 4U);
  EXPECT_EQ(schema->header_line(), "symbol\tdate\tf0_log_rv1\tlabel");
  const auto idx = schema->index_of("f0_log_rv1");
  ASSERT_TRUE(idx);
  EXPECT_EQ(*idx, 2U);
  EXPECT_FALSE(schema->index_of("nope"));
}

TEST(AlphaSchema, StripsCommentMarkerAndCarriageReturn) {
  const auto schema = PanelSchema::from_header("# symbol\tdate\tlabel\r\n");
  ASSERT_TRUE(schema) << schema.error().message();
  EXPECT_EQ(schema->size(), 3U);
  EXPECT_TRUE(schema->has("symbol"));
  EXPECT_TRUE(schema->has("label"));
}

TEST(AlphaSchema, RejectsDuplicateColumns) {
  const auto schema = PanelSchema::from_header("a\tb\ta");
  ASSERT_FALSE(schema);
  EXPECT_EQ(schema.error().code(), atx::core::ErrorCode::AlreadyExists);
}

TEST(AlphaSchema, RequireReportsEveryMissingColumnAtOnce) {
  const auto schema = PanelSchema::from_header("symbol\tdate\tf0_log_rv1");
  ASSERT_TRUE(schema);
  const auto got = schema->require({"symbol", "f9_vov_63d", "liq_hspread_frac"});
  ASSERT_FALSE(got);
  const std::string &msg = got.error().message();
  EXPECT_NE(msg.find("f9_vov_63d"), std::string::npos);
  EXPECT_NE(msg.find("liq_hspread_frac"), std::string::npos) << msg;
}

TEST(AlphaSchema, FingerprintDependsOnOrderAndOnContent) {
  const auto a = PanelSchema::from_header("a\tb\tc");
  const auto b = PanelSchema::from_header("a\tc\tb");
  const auto c = PanelSchema::from_header("a\tb\tc\td");
  const auto d = PanelSchema::from_header("a\tb\tc");
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  ASSERT_TRUE(c);
  ASSERT_TRUE(d);
  EXPECT_NE(a->fingerprint(), b->fingerprint());
  EXPECT_NE(a->fingerprint(), c->fingerprint());
  EXPECT_EQ(a->fingerprint(), d->fingerprint());
  // The separator is what stops {"ab","c"} colliding with {"a","bc"}.
  const auto e = PanelSchema::from_header("ab\tc");
  const auto f = PanelSchema::from_header("a\tbc");
  ASSERT_TRUE(e);
  ASSERT_TRUE(f);
  EXPECT_NE(e->fingerprint(), f->fingerprint());
}

TEST(AlphaSchema, ANewColumnIsAdditiveForAnOldReader) {
  // The whole point of resolving by name: a v3-shaped reader keeps working on
  // a panel that gained twelve columns, with no version branch anywhere.
  const auto old_reader_wants =
      std::vector<std::string>{"symbol", "date", "label", "f4_term_slope"};
  const auto wide = PanelSchema::from_header(
      "symbol\tdate\tlabel\tf4_term_slope\tf16_iv_vov_21d\tliq_hspread_frac");
  ASSERT_TRUE(wide);
  const auto idx = wide->require(old_reader_wants);
  ASSERT_TRUE(idx) << idx.error().message();
  EXPECT_EQ(idx->size(), 4U);
  EXPECT_EQ((*idx)[3], 3U);
}

// ── The adjudicator: cases that must FIRE ───────────────────────────────────

TEST(AlphaAudit, LabelCarriesTheEntryIvMarkAndTheIvLevelFeatureSharesIt) {
  // Rounds 1-4 headlined `(rv_fwd^2 - iv_fair^2)*H` and read predictors that
  // themselves contain iv_fair[t]. That is an entry-mark channel, not a leak.
  const FeatureRegistry freg = builtin_features();
  const auto treg = builtin_targets();
  const AuditReport rep =
      run_audit({feat(freg, "f3_iv_level")}, targ(treg, "label_contaminated"));
  EXPECT_EQ(rep.count(FindingKind::EntryMarkChannel), 1U);
  EXPECT_FALSE(rep.has_fatal()) << "an entry-mark channel is tradeable, never fatal";
}

TEST(AlphaAudit, RoundNineButterflyCarriesTheEntryMarkAgainstAnIvChangeAxis) {
  // `f12_bf25_21d` contains -iv_atmf_21d[t]; `iv_chg_21d_raw` contains
  // +iv_atmf_21d[t]. Round 9 documented this in prose. Here it is computed.
  const FeatureRegistry freg = builtin_features();
  const auto treg = builtin_targets();
  const AuditReport rep =
      run_audit({feat(freg, "f12_bf25_21d")}, targ(treg, "iv_chg_21d_raw"));
  EXPECT_EQ(rep.count(FindingKind::EntryMarkChannel), 1U);
  // Its two wing legs are a different read of the same tenor and session:
  // reported as Info, not as a shared term.
  EXPECT_EQ(rep.count(FindingKind::CorrelatedEntryMark), 2U);
}

TEST(AlphaAudit, OneSessionOfFeatureLagClosesTheEntryMarkChannel) {
  // Round 9 asserts in prose that the butterfly "must not be read at lag 0".
  // How much lag is enough is arithmetic on the windows, and the answer is
  // ONE: every one of these features ends its window at t, and the target's
  // entry leg is the single session t, so shifting the window one session into
  // the past removes the only shared session. (That is a different question
  // from `--eiv-target-entry-lag 2`, which exists to control errors-in-
  // variables in the TARGET rebuild, not to break a shared term.)
  const FeatureRegistry freg = builtin_features();
  const auto treg = builtin_targets();
  const TargetSpec &target = targ(treg, "iv_chg_21d_raw");
  const std::vector<const FeatureSpec *> set{feat(freg, "f12_bf25_21d"),
                                             feat(freg, "f14_iv_chg_5d")};
  EXPECT_EQ(run_audit(set, target, /*lag=*/0).count(FindingKind::EntryMarkChannel), 2U);
  const AuditReport lagged = run_audit(set, target, /*lag=*/1);
  EXPECT_EQ(lagged.count(FindingKind::EntryMarkChannel), 0U);
  EXPECT_EQ(lagged.count(FindingKind::CorrelatedEntryMark), 0U);
  EXPECT_FALSE(lagged.has_fatal());
  EXPECT_TRUE(run_audit(set, target, /*lag=*/2).clean());
}

TEST(AlphaAudit, VolChgCarriesAnExplicitTrailingRealizedLeg) {
  // `tools/vrp_train.hpp` calls vol_chg "the CLEANEST axis in the set: no
  // iv_fair anywhere in it and no trivial trailing-vol persistence". The first
  // clause holds. The second does not: ln(rv_fwd/rv_trail) has a -ln(rv_trail)
  // leg, so every trailing-realized feature is mechanically related to it.
  const FeatureRegistry freg = builtin_features();
  const auto treg = builtin_targets();
  const AuditReport rep =
      run_audit({feat(freg, "f2_log_rv21")}, targ(treg, "vol_chg_21d"));
  EXPECT_EQ(rep.count(FindingKind::EntryMarkChannel), 1U);
  // ... and the first clause really does hold: an iv-only feature is clean.
  const AuditReport iv_only =
      run_audit({feat(freg, "f3_iv_level")}, targ(treg, "vol_chg_21d"));
  EXPECT_EQ(iv_only.count(FindingKind::EntryMarkChannel), 0U);
}

TEST(AlphaAudit, TheMoneyAxisCarriesTheChannelByConstruction) {
  // A delta-hedged straddle pays ~(rv_fwd - iv_entry). Every feature that reads
  // the entry IV mark therefore scores against it partly through the channel.
  // That is the TRADE, not a defect -- so it must be Warn and never Fatal, and
  // the remedy is the cross-read, not deletion.
  const FeatureRegistry freg = builtin_features();
  const auto treg = builtin_targets();
  const AuditReport rep =
      run_audit({feat(freg, "f5_hv_iv_gap")}, targ(treg, "dh_straddle_pnl_21d"));
  EXPECT_EQ(rep.count(FindingKind::EntryMarkChannel), 1U);
  EXPECT_FALSE(rep.has_fatal());
  const auto subjects = rep.subjects(FindingKind::EntryMarkChannel);
  ASSERT_EQ(subjects.size(), 1U);
  EXPECT_EQ(subjects[0], "f5_hv_iv_gap");
}

TEST(AlphaAudit, AForwardReadingFeatureIsFatalAndNamesTheTargetWindow) {
  const auto treg = builtin_targets();
  const FeatureSpec peeker{"peek_fwd_5d", Unit::LogReturn, SignPrior::None, "",
                           {spot(0, 5)}, "reads five sessions past entry"};
  const AuditReport rep = run_audit({&peeker}, targ(treg, "rv_fwd_21d"));
  EXPECT_TRUE(rep.has_fatal());
  EXPECT_EQ(rep.count(FindingKind::TargetWindowLeak), 1U);
}

TEST(AlphaAudit, AForwardReadOutsideTheTargetWindowIsStillFatal) {
  const auto treg = builtin_targets();
  // Reads forward SPOT, but the target is an IV-change axis that reads no spot
  // at all: still lookahead, just not "reading the answer".
  const FeatureSpec peeker{"peek_fwd_5d", Unit::LogReturn, SignPrior::None, "",
                           {spot(1, 5)}, "reads five sessions past entry"};
  const AuditReport rep = run_audit({&peeker}, targ(treg, "iv_chg_21d_raw"));
  EXPECT_TRUE(rep.has_fatal());
  EXPECT_EQ(rep.count(FindingKind::ForwardLeak), 1U);
  EXPECT_EQ(rep.count(FindingKind::TargetWindowLeak), 0U);
}

TEST(AlphaAudit, HeadliningAForecastAxisIsReported) {
  const FeatureRegistry freg = builtin_features();
  const auto treg = builtin_targets();
  const AuditReport as_headline = run_audit({feat(freg, "f4_term_slope")},
                                            targ(treg, "rv_fwd_21d"), 0, /*headline=*/true);
  EXPECT_EQ(as_headline.count(FindingKind::NonTradeableTarget), 1U);
  const AuditReport alongside =
      run_audit({feat(freg, "f4_term_slope")}, targ(treg, "rv_fwd_21d"), 0, /*headline=*/false);
  EXPECT_EQ(alongside.count(FindingKind::NonTradeableTarget), 0U);
  // A tradeable axis in the headline seat is silent.
  const AuditReport tradeable = run_audit({feat(freg, "f4_term_slope")},
                                          targ(treg, "iv_chg_21d_roll"), 0, /*headline=*/true);
  EXPECT_EQ(tradeable.count(FindingKind::NonTradeableTarget), 0U);
}

TEST(AlphaAudit, IdenticalInputFootprintsAreFlagged) {
  const FeatureSpec a{"vov_a", Unit::VolDecimal, SignPrior::None, "",
                      {atx::vol::alpha::atmf(21, -21, 0)}, "x"};
  const FeatureSpec b{"vov_b", Unit::VolDecimal, SignPrior::None, "",
                      {atx::vol::alpha::atmf(21, -21, 0)}, "y"};
  const auto treg = builtin_targets();
  const AuditReport rep = run_audit({&a, &b}, targ(treg, "rv_fwd_21d"));
  EXPECT_EQ(rep.count(FindingKind::SharedInputFootprint), 1U);
  EXPECT_FALSE(rep.has_fatal()) << "same footprint is a lead, not a verdict";
  // f16 and f20 differ in WINDOW, so the catalogue must be clean on that pair.
  const FeatureRegistry freg = builtin_features();
  const AuditReport cat = run_audit({feat(freg, "f16_iv_vov_21d"), feat(freg, "f20_iv_vov_63d")},
                                    targ(treg, "rv_fwd_21d"));
  EXPECT_EQ(cat.count(FindingKind::SharedInputFootprint), 0U);
}

TEST(AlphaAudit, TheCatalogueCarriesExactlyTwoSharedFootprintPairs) {
  // A MEASUREMENT of the shipped feature set, pinned so a tenth round has to
  // restate it rather than drift past a reviewer:
  //
  //   f2_log_rv21 / f7_ret_21d   -- one window of spot, two genuinely different
  //     transforms (log variance vs summed return). Keep both.
  //   f5_hv_iv_gap / f6_vrp_lag  -- the log-RATIO and the DIFFERENCE of exactly
  //     the same two quantities (trailing realized variance, entry iv^2). These
  //     are close to a monotone re-expression of each other and the panel has
  //     carried both since round 1 without that being written down anywhere.
  const FeatureRegistry freg = builtin_features();
  const auto treg = builtin_targets();
  std::vector<const FeatureSpec *> all;
  for (const FeatureSpec &spec : freg.all()) {
    all.push_back(&spec);
  }
  //
  // ROUND 11 restated it, 2 pairs -> 17. Two new clusters:
  //   f2/f7/f22/f23/f24/f25 -- all six read exactly spot[-21..0]. This one is
  //     ON PURPOSE and must not be "fixed" by merging them: Patton & Sheppard's
  //     entire claim is that upside and downside semivariance carry DIFFERENT
  //     coefficients over the SAME window (0.091 vs 0.388 at h=22). A
  //     decomposition whose parts did not share a footprint would not be a
  //     decomposition.
  //   f15/f27 -- idio_share and sysvol_share read the same 63-session
  //     (spot, market) pair and are literally 1 - each other. They ship as two
  //     because Cao & Han report them with OPPOSITE SIGNS in one regression.
  //
  // The earnings family restated it again, 17 -> 18: f28/f29 both read exactly
  // {event@[0,0]} -- one schedule snapshot, transformed as days-to-print vs
  // prints-in-window. Deliberate, same rationale as the semivariance block.
  // f30 pairs with NEITHER: its read set adds the two implied strips, and the
  // census fires on full-set equality, not overlap.
  const AuditReport rep = run_audit(all, targ(treg, "rv_fwd_21d"));
  EXPECT_EQ(rep.count(FindingKind::SharedInputFootprint), 18U);
  const std::vector<std::string> subjects = rep.subjects(FindingKind::SharedInputFootprint);
  const std::vector<std::string> expected{"f2_log_rv21",        "f5_hv_iv_gap",
                                          "f7_ret_21d",         "f15_idio_share",
                                          "f22_semivar_dn_21d", "f23_semivar_up_21d",
                                          "f24_signed_jump_21d", "f28_days_to_earn"};
  EXPECT_EQ(subjects, expected);
}

TEST(AlphaAudit, WarmupShortfallCountsTheLag) {
  const FeatureRegistry freg = builtin_features();
  const auto treg = builtin_targets();
  AuditConfig cfg;
  cfg.panel_warmup_sessions = 252;
  cfg.feature_lag = 2;
  // f10 needs 251 trailing sessions; with two of lag it needs 253.
  const AuditReport rep =
      atx::vol::alpha::audit(std::vector<const FeatureSpec *>{feat(freg, "f10_iv_rank_252")},
                             targ(treg, "rv_fwd_21d"), cfg);
  EXPECT_EQ(rep.count(FindingKind::InsufficientWarmup), 1U);
  cfg.feature_lag = 0;
  const AuditReport fits =
      atx::vol::alpha::audit(std::vector<const FeatureSpec *>{feat(freg, "f10_iv_rank_252")},
                             targ(treg, "rv_fwd_21d"), cfg);
  EXPECT_EQ(fits.count(FindingKind::InsufficientWarmup), 0U);
}

// ── The adjudicator: cases that must stay SILENT ────────────────────────────

TEST(AlphaAudit, GoyalSarettoAgainstTheRealizedAxisIsClean) {
  // f5 reads trailing spot and the entry IV mark; rv_fwd_21d reads FORWARD
  // spot only. No shared session, no shared series-at-a-session. This is the
  // control: an adjudicator that flags this is worthless.
  const FeatureRegistry freg = builtin_features();
  const auto treg = builtin_targets();
  const AuditReport rep = run_audit({feat(freg, "f5_hv_iv_gap")}, targ(treg, "rv_fwd_21d"));
  EXPECT_TRUE(rep.clean()) << (rep.findings.empty() ? "" : rep.findings.front().detail);
}

TEST(AlphaAudit, TheVasquezSlopeFamilyIsCleanAgainstEveryRealizedAxis) {
  const FeatureRegistry freg = builtin_features();
  const auto treg = builtin_targets();
  const std::vector<std::string> patterns{"f4_term_slope", "f1?_slope_*"};
  const auto sel = freg.select(patterns);
  ASSERT_TRUE(sel) << sel.error().message();
  ASSERT_EQ(sel->size(), 4U);
  const AuditReport rep = run_audit(*sel, targ(treg, "rv_fwd_21d"));
  EXPECT_TRUE(rep.clean());
}

TEST(AlphaAudit, CrossReadPicksAnAxisWithoutTheSharedLeg) {
  const FeatureRegistry freg = builtin_features();
  const auto treg = builtin_targets();
  const std::vector<const FeatureSpec *> set{feat(freg, "f5_hv_iv_gap")};
  const std::string axis = atx::vol::alpha::cross_read_axis(
      set, treg.all(), targ(treg, "dh_straddle_pnl_21d"), /*feature_lag=*/0);
  EXPECT_EQ(axis, "rv_fwd_21d");
}

TEST(AlphaAudit, FormatReportIsOneStableLinePerFinding) {
  const FeatureRegistry freg = builtin_features();
  const auto treg = builtin_targets();
  const AuditReport rep =
      run_audit({feat(freg, "f12_bf25_21d")}, targ(treg, "iv_chg_21d_raw"));
  const std::vector<std::string> lines = atx::vol::alpha::format_report(rep);
  ASSERT_EQ(lines.size(), rep.findings.size());
  ASSERT_FALSE(lines.empty());
  bool saw_channel = false;
  for (const std::string &line : lines) {
    if (line.rfind("WARN entry_mark_channel f12_bf25_21d: ", 0) == 0) {
      saw_channel = true;
    }
  }
  EXPECT_TRUE(saw_channel) << lines.front();
}

// ── The whole catalogue, adjudicated ────────────────────────────────────────

TEST(AlphaAudit, NoCataloguedFeatureLeaksAgainstAnyCataloguedTarget) {
  // The standing invariant: the shipped feature set may carry channels (they
  // are priced and tradeable), but it must never carry lookahead.
  const FeatureRegistry freg = builtin_features();
  const auto treg = builtin_targets();
  std::vector<const FeatureSpec *> all;
  for (const FeatureSpec &spec : freg.all()) {
    all.push_back(&spec);
  }
  for (const TargetSpec &target : treg.all()) {
    const AuditReport rep = run_audit(all, target);
    EXPECT_FALSE(rep.has_fatal()) << "fatal finding against target '" << target.name << "'";
    // Shared input footprints are Info and are pinned by their own test; the
    // standing invariant here is only that nothing LEAKS.
    // 18 as of the round-11 earnings family, up from 17, up from 2. The 17 is
    // the semivariance block: f22, f23, f24 and f25 all read exactly
    // spot[-21..0], the same footprint as f2 and f7, so the six of them
    // contribute C(6,2) = 15 pairs, plus the long-standing f5/f6 and
    // f2/f7-family pairs. That is the finding working, not a defect --
    // Patton & Sheppard's whole claim is that RS+ and RS- carry DIFFERENT
    // coefficients over the SAME window, so a decomposition that did not
    // share a footprint would not be their decomposition.
    // The 18th is f28/f29: both read exactly {event@[0,0]} -- the same
    // schedule snapshot transformed two ways (proximity vs in-window count).
    // f30 does NOT pair with them: its reads also carry the two implied
    // strips, and the census fires on full-set EQUALITY, not overlap.
    EXPECT_EQ(rep.count(FindingKind::SharedInputFootprint), 18U)
        << "the shared-footprint census moved against target '" << target.name << "'";
  }
}

TEST(AlphaAudit, TheChannelCensusIsTheNumberTheGateShouldPrint) {
  // Not an invariant -- a MEASUREMENT, pinned so that a change to the
  // catalogue has to restate it rather than drift past a reviewer. Against the
  // delta-hedged money axis, every feature reading iv_fair@21d[t] is in a
  // channel: f3, f4, f5, f6, f13, f17, f18, f19 -- and f9, whose 63-session
  // vol-of-vol window INCLUDES session t, which a reader scanning the feature
  // list for "reads iv at entry" would miss because the entry read is buried
  // inside a rolling stdev.
  //
  // ROUND 11 adds f26_gs_hviv_252d, and that addition is the most useful thing
  // this census has produced. f26 is the AS-PUBLISHED Goyal & Saretto signal,
  // ln(rv_252 / iv_21) -- so it is ln(rv_252) MINUS ln(iv_21), and the entry
  // mark is inside it by construction. A feature can be the single most cited
  // predictor in the literature and still be, mechanically, partly a bet that
  // the entry mark is low. It stays in the catalogue and it stays flagged: the
  // remedy for a channel is the decontaminated cross-read, never deletion.
  //
  // The earnings family adds f30_earn_sigma_e: the extraction reads both
  // implied strips AT t (they are its sigma1/sigma2 inputs), so against a
  // target that pays -iv^2 it inherits exactly f4's channel, reweighted by
  // event placement. f28 and f29 read only the schedule and stay OUT of the
  // list -- an earnings date carries no entry mark. f31_earn_move_rich is
  // sigma_E over the name's delivered history, so it carries f30's strips
  // and the same channel.
  const FeatureRegistry freg = builtin_features();
  const auto treg = builtin_targets();
  std::vector<const FeatureSpec *> all;
  for (const FeatureSpec &spec : freg.all()) {
    all.push_back(&spec);
  }
  const AuditReport rep = run_audit(all, targ(treg, "dh_straddle_pnl_21d"));
  const std::vector<std::string> subjects = rep.subjects(FindingKind::EntryMarkChannel);
  const std::vector<std::string> expected{
      "f3_iv_level",      "f4_term_slope",    "f5_hv_iv_gap",   "f6_vrp_lag",
      "f9_vov_63d",       "f13_term_curv",    "f17_slope_126d", "f18_slope_189d",
      "f19_slope_252d",   "f26_gs_hviv_252d", "f30_earn_sigma_e",
      "f31_earn_move_rich"};
  EXPECT_EQ(subjects, expected);
}


// ── PanelFrame ──────────────────────────────────────────────────────────────

namespace {
PanelFrame load(const std::string &text) {
  std::istringstream in(text);
  auto got = PanelFrame::read_tsv(in);
  EXPECT_TRUE(got) << (got ? "" : got.error().to_string());
  return got ? std::move(*got) : PanelFrame{};
}
} // namespace

TEST(AlphaFrame, InfersNumericAndTextColumns) {
  const PanelFrame f = load("symbol\tdate\tf3_iv_level\n"
                            "AAPL\t2026-01-02\t-1.5\n"
                            "MSFT\t2026-01-02\t-1.25\n");
  EXPECT_EQ(f.rows(), 2U);
  EXPECT_EQ(f.cols(), 3U);
  const auto syms = f.strings("symbol");
  ASSERT_TRUE(syms);
  EXPECT_EQ((*syms)[0], "AAPL");
  const auto vals = f.numbers("f3_iv_level");
  ASSERT_TRUE(vals);
  EXPECT_DOUBLE_EQ((*vals)[1], -1.25);
  // Asking for the wrong kind is an error, not a silent empty span.
  EXPECT_FALSE(f.numbers("symbol"));
  EXPECT_FALSE(f.strings("f3_iv_level"));
}

TEST(AlphaFrame, NanIsNumericAndCountedSeparately) {
  // The panel writes the canonical spelling "nan" for a warmup row and for an
  // unavailable strip. It must stay in the numeric column, not demote it.
  const PanelFrame f = load("date\tf9_vov_63d\n"
                            "2026-01-02\tnan\n"
                            "2026-01-05\t0.045\n"
                            "2026-01-06\tnan\n");
  const auto st = f.column_stats("f9_vov_63d");
  ASSERT_TRUE(st);
  EXPECT_TRUE((*st)->numeric);
  EXPECT_EQ((*st)->n_finite, 1U);
  EXPECT_EQ((*st)->n_nan, 2U);
  EXPECT_NEAR((*st)->finite_fraction(), 1.0 / 3.0, 1e-12);
}

TEST(AlphaFrame, RetainsMetaLinesAndStillFindsTheHeader) {
  const PanelFrame f = load("# schema=vrp_panel_v2\n"
                            "# horizon_days=21\n"
                            "symbol\tlabel\n"
                            "AAPL\t0.01\n");
  ASSERT_EQ(f.meta().size(), 2U);
  EXPECT_EQ(f.meta()[0], "# schema=vrp_panel_v2");
  EXPECT_EQ(f.rows(), 1U);
  EXPECT_TRUE(f.schema().has("label"));
}

TEST(AlphaFrame, AShortRowIsAnErrorNotAPad) {
  // A row that disagrees with the header means writer and reader disagree
  // about the schema. Padding it with NaN is how that survives to a fit.
  std::istringstream in("a\tb\tc\n1\t2\n");
  const auto got = PanelFrame::read_tsv(in);
  ASSERT_FALSE(got);
  EXPECT_EQ(got.error().code(), atx::core::ErrorCode::ParseError);
}

TEST(AlphaFrame, ALongRowIsAlsoAnError) {
  std::istringstream in("a\tb\n1\t2\t3\n");
  const auto got = PanelFrame::read_tsv(in);
  ASSERT_FALSE(got);
  EXPECT_EQ(got.error().code(), atx::core::ErrorCode::ParseError);
}

TEST(AlphaFrame, FlagsAllMissingAndConstantColumns) {
  const PanelFrame f = load("date\tdead\tflat\tlive\n"
                            "2026-01-02\tnan\t1\t0.5\n"
                            "2026-01-05\tnan\t1\t0.7\n");
  const std::vector<std::string> unusable = f.unusable_columns();
  ASSERT_EQ(unusable.size(), 2U);
  EXPECT_EQ(unusable[0], "dead");
  EXPECT_EQ(unusable[1], "flat");
  const auto live = f.column_stats("live");
  ASSERT_TRUE(live);
  EXPECT_FALSE((*live)->all_missing());
  EXPECT_FALSE((*live)->constant());
}

TEST(AlphaFrame, ANewColumnDoesNotDisturbAnOlderReader) {
  // The end-to-end statement of the whole layer: a panel that gained twelve
  // columns is still read by code that only knows four, with no version
  // branch, no recompile, and no column-count constant.
  const PanelFrame wide = load(
      "symbol\tdate\tlabel\tf4_term_slope\tf16_iv_vov_21d\tliq_hspread_frac\n"
      "AAPL\t2026-01-02\t0.01\t-0.02\t0.11\t0.004\n");
  const auto idx = wide.schema().require({"symbol", "date", "label", "f4_term_slope"});
  ASSERT_TRUE(idx) << idx.error().message();
  const auto slope = wide.numbers("f4_term_slope");
  ASSERT_TRUE(slope);
  EXPECT_DOUBLE_EQ((*slope)[0], -0.02);
}

} // namespace
