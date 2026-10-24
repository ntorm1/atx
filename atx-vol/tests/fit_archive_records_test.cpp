// ATXVFA typed records: bit-exact round trips for every stored record type
// (write -> read -> compare bytes and struct fields), zero-copy serving with no
// allocation after open, and refusals of corrupt record payloads.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/fitting/american_markov_fit.hpp"
#include "atx/vol/api/fitting/expiry_record.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_markov.hpp"
#include "atx/vol/api/pricing/chain_tick.hpp"
#include "atx/vol/api/storage/fit_archive.hpp"
#include "atx/vol/api/storage/fit_archive_records.hpp"
#include "support/alloc_counter.hpp"
#include "support/fit_archive_fixture.hpp"

namespace atx::vol {
namespace {

using testkit::bits;
using testkit::diff_slice;
using testkit::find_section;
using testkit::join_image;
using testkit::open_status;
using testkit::split_image;
using Type = FitArchiveSectionType;

constexpr std::int64_t kSnapshot = 1791988200000000000LL;

TEST(FitArchiveRecords, UnpersistedTerminalFloorPolicyIsExplicitlyRefused) {
  auto surface = testkit::synthetic_surface();
  ASSERT_GT(surface.slices.size(), 1u);
  surface.slices[1].fit.terminal_floor_recovery = true;
  FitArchiveWriter standalone;
  const auto fit = standalone.add_fit({"SPY", kSnapshot, 20800, 0, &surface.slices[1].fit});
  ASSERT_FALSE(fit);
  EXPECT_EQ(fit.error().status, ArchiveStatus::UnsupportedRecordVersion);
  EXPECT_EQ(fit.error().section, static_cast<std::uint32_t>(Type::FitRecords));
  FitArchiveWriter grouped;
  const auto all = grouped.add_surface({"SPY", kSnapshot, surface.slices,
                                       surface.expiry_days, surface.config, {}, "", ""});
  ASSERT_FALSE(all);
  EXPECT_EQ(all.error().status, ArchiveStatus::UnsupportedRecordVersion);
  EXPECT_EQ(all.error().section, static_cast<std::uint32_t>(Type::FitRecords));
  // Both entry points roll back; the writer stays reusable for supported fits.
  surface.slices[1].fit.terminal_floor_recovery = false;
  ASSERT_TRUE(standalone.add_fit({"SPY", kSnapshot, 20800, 0, &surface.slices[1].fit}));
  ASSERT_TRUE(grouped.add_surface({"SPY", kSnapshot, surface.slices,
                                  surface.expiry_days, surface.config, {}, "", ""}));
  const auto standalone_image = standalone.finish();
  const auto grouped_image = grouped.finish();
  ASSERT_TRUE(standalone_image && grouped_image);
  const auto standalone_read = FitArchive::open_copy(*standalone_image);
  const auto grouped_read = FitArchive::open_copy(*grouped_image);
  ASSERT_TRUE(standalone_read && grouped_read);
  EXPECT_EQ(standalone_read->fits().size(), 1u);
  EXPECT_EQ(grouped_read->fits().size(), surface.slices.size());
}

TEST(FitArchiveRecords, UnpersistedRepricingBudgetIsRefusedAndBothWritersRollBack) {
  const std::array<std::array<double, 2>, 3> budgets{{
      {0.001, 0.01}, {0.001, 0.0}, {0.0, 0.01}}};
  for (const auto &budget : budgets) {
    auto surface = testkit::synthetic_surface();
    ASSERT_GT(surface.slices.size(), 1u);
    auto &slice = surface.slices[1].fit;
    slice.reimply_price_cap = budget[0];
    slice.reimply_spread_fraction = budget[1];
    FitArchiveWriter standalone;
    const auto fit = standalone.add_fit({"SPY", kSnapshot, 20800, 0, &slice});
    ASSERT_FALSE(fit);
    EXPECT_EQ(fit.error().status, ArchiveStatus::UnsupportedRecordVersion);
    EXPECT_EQ(fit.error().section, static_cast<std::uint32_t>(Type::FitRecords));
    FitArchiveWriter grouped;
    const auto all = grouped.add_surface({"SPY", kSnapshot, surface.slices,
                                         surface.expiry_days, surface.config, {}, "", ""});
    ASSERT_FALSE(all);
    EXPECT_EQ(all.error().status, ArchiveStatus::UnsupportedRecordVersion);
    slice.reimply_price_cap = 0.0;
    slice.reimply_spread_fraction = 0.0;
    ASSERT_TRUE(standalone.add_fit({"SPY", kSnapshot, 20800, 0, &slice}));
    ASSERT_TRUE(grouped.add_surface({"SPY", kSnapshot, surface.slices,
                                    surface.expiry_days, surface.config, {}, "", ""}));
    const auto one = standalone.finish();
    const auto many = grouped.finish();
    ASSERT_TRUE(one && many);
    const auto one_read = FitArchive::open_copy(*one);
    const auto many_read = FitArchive::open_copy(*many);
    ASSERT_TRUE(one_read && many_read);
    EXPECT_EQ(one_read->fits().size(), 1u);
    EXPECT_EQ(many_read->fits().size(), surface.slices.size());
  }
}

TEST(FitArchiveRecords, UnpersistedSupportAndFinalPriceMetadataRefuseWithoutPartialWrites) {
  for (const unsigned field : {0u, 1u, 2u, 3u}) {
    SCOPED_TRACE(field);
    auto surface = testkit::synthetic_surface();
    ASSERT_GT(surface.slices.size(), 1u);
    auto &fit = surface.slices[1].fit;
    ASSERT_TRUE(fit.prices.has_value());
    ASSERT_FALSE(fit.rows.empty());
    const auto original_status = fit.rows.front().quote_status;
    if (field == 0u) fit.withheld_nodes.assign(fit.prices->nodes().size(), 3u);
    if (field == 1u) fit.diagnostics.final_price_observations = 1u;
    if (field == 2u) fit.diagnostics.final_price_identified_nodes = 1u;
    if (field == 3u) fit.rows.front().quote_status = QuoteSliceQuoteStatus::ProjectedPrice;
    FitArchiveWriter standalone;
    FitArchiveWriter grouped;
    const auto one = standalone.add_fit({"SPY", kSnapshot, 20800, 0, &fit});
    const auto many = grouped.add_surface({"SPY", kSnapshot, surface.slices,
                                          surface.expiry_days, surface.config, {}, "", ""});
    ASSERT_FALSE(one);
    ASSERT_FALSE(many);
    EXPECT_EQ(one.error().status, ArchiveStatus::UnsupportedRecordVersion);
    EXPECT_EQ(many.error().status, ArchiveStatus::UnsupportedRecordVersion);
    EXPECT_EQ(one.error().section, static_cast<std::uint32_t>(Type::FitRecords));
    EXPECT_EQ(standalone.fit_count(), 0u);
    EXPECT_EQ(grouped.fit_count(), 0u);
    EXPECT_EQ(grouped.surface_count(), 0u);
    fit.withheld_nodes.clear();
    fit.diagnostics.final_price_observations = 0u;
    fit.diagnostics.final_price_identified_nodes = 0u;
    fit.rows.front().quote_status = original_status;
    ASSERT_TRUE(standalone.add_fit({"SPY", kSnapshot, 20800, 0, &fit}));
    ASSERT_TRUE(grouped.add_surface({"SPY", kSnapshot, surface.slices,
                                     surface.expiry_days, surface.config, {}, "", ""}));
    const auto one_image = standalone.finish();
    const auto many_image = grouped.finish();
    ASSERT_TRUE(one_image && many_image);
    const auto one_read = FitArchive::open_copy(*one_image, FitArchiveVerify::Paranoid);
    const auto many_read = FitArchive::open_copy(*many_image, FitArchiveVerify::Paranoid);
    ASSERT_TRUE(one_read && many_read);
    EXPECT_EQ(one_read->fits().size(), 1u);
    EXPECT_EQ(many_read->fits().size(), surface.slices.size());
  }
}

std::array<std::uint8_t, 32> digest() {
  std::array<std::uint8_t, 32> d{};
  for (std::size_t i = 0; i < d.size(); ++i) d[i] = static_cast<std::uint8_t>(0xd7 ^ i);
  return d;
}

FitArchive open_image(const std::vector<std::byte> &image) {
  auto a = FitArchive::open_copy(image, FitArchiveVerify::Paranoid);
  if (!a) {
    ADD_FAILURE() << to_string(a.error().status);
    return std::move(*FitArchive::open_copy(*FitArchiveWriter{}.finish()));
  }
  return std::move(*a);
}

// The comparator the round trips rely on must see a one-ulp / one-field change,
// and the synthetic surface must actually serve (or every comparison is vacuous).
TEST(FitArchiveRecords, FixtureComparisonDetectsSingleFieldChanges) {
  const auto surface = testkit::synthetic_surface();
  const auto served = std::count_if(surface.slices.begin(), surface.slices.end(), [](const auto &s) {
    return s.grade != QuoteSliceGrade::Refused && s.fit.prices.has_value() && s.fit.curve != nullptr &&
           !s.fit.rows.empty();
  });
  EXPECT_GE(served, 2);
  const auto base = testkit::exotic_slice(surface.slices[1], VolCurveKind::ConvexDense);
  auto same = testkit::exotic_slice(surface.slices[1], VolCurveKind::ConvexDense);
  EXPECT_EQ(diff_slice(base, same), "");
  auto row = testkit::exotic_slice(surface.slices[1], VolCurveKind::ConvexDense);
  row.fit.rows[0].fitted_price = std::nextafter(row.fit.rows[0].fitted_price, 1.0e9);
  EXPECT_EQ(diff_slice(base, row), "rows");
  auto carry = testkit::exotic_slice(surface.slices[1], VolCurveKind::ConvexDense);
  carry.fit.carry_resolution.pairs_unidentified += 1;
  EXPECT_EQ(diff_slice(base, carry), "carry resolution");
  auto curve = testkit::exotic_slice(surface.slices[1], VolCurveKind::ConvexDense);
  ConvexSliceFit params = static_cast<const ConvexDenseCurve &>(*curve.fit.curve).fit();
  params.n_active += 1;
  curve.fit.curve = std::make_unique<ConvexDenseCurve>(std::move(params));
  EXPECT_EQ(diff_slice(base, curve), "convex-dense params");
  auto flag = testkit::exotic_slice(surface.slices[1], VolCurveKind::ConvexDense);
  flag.calendar_readmitted = false;
  EXPECT_EQ(diff_slice(base, flag), "slice flags");
  auto po = testkit::exotic_slice(surface.slices[1], VolCurveKind::ConvexDense);
  ASSERT_FALSE(po.fit.price_only_nodes.empty());
  po.fit.price_only_nodes[0] = 0;
  EXPECT_EQ(diff_slice(base, po), "price_only_nodes");
  auto snap = testkit::exotic_slice(surface.slices[1], VolCurveKind::ConvexDense);
  snap.fit.snapshot.spot_version += 1;
  EXPECT_EQ(diff_slice(base, snap), "snapshot");
  auto rule = testkit::exotic_slice(surface.slices[1], VolCurveKind::ConvexDense);
  rule.fit.snapshot.spot_rule = QuoteSliceSpotRule::Median;
  EXPECT_EQ(diff_slice(base, rule), "snapshot");
  auto reason = testkit::exotic_slice(surface.slices[1], VolCurveKind::ConvexDense);
  reason.fit.rows[0].price_only_reason = QuoteSlicePriceOnlyReason::LowVega;
  EXPECT_EQ(diff_slice(base, reason), "rows");
}

TEST(FitArchiveRecords, GradedSurfaceRoundTripIsBitExact) {
  const auto surface = testkit::synthetic_surface();
  ASSERT_EQ(surface.slices.size(), 3u);
  FitArchiveWriter w;
  w.record_timings(true); // bit-exact including the wall-clock solve time
  ASSERT_TRUE(w.add_surface({"AAPL", kSnapshot, surface.slices, surface.expiry_days, surface.config, digest(),
                             "ordinary", "raw-liquidity"})
                  .has_value());
  const auto dir = testkit::scratch_dir("surface");
  const auto path = (dir / "s.atxvfa").string();
  ASSERT_TRUE(w.write_file(path).has_value());
  for (const auto mode : {FitArchiveOpenMode::Mapped, FitArchiveOpenMode::Read}) {
    auto a = FitArchive::open(path, {mode, FitArchiveVerify::Paranoid});
    ASSERT_TRUE(a.has_value()) << to_string(a.error().status);
    const auto id = a->find_underlier("AAPL");
    ASSERT_TRUE(id.has_value());
    const FitArchiveSurfaceRecord *s = a->find_surface(*id, kSnapshot);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->policy_digest, digest());
    EXPECT_EQ(a->string(s->profile), "ordinary");
    EXPECT_EQ(a->string(s->policy_source), "raw-liquidity");
    const QuoteSurfaceConfig c = decode_surface_config(*s);
    EXPECT_TRUE(bits(c.calendar_repair_half_spreads, surface.config.calendar_repair_half_spreads));
    EXPECT_EQ(c.grade_b_min_mid_strikes, surface.config.grade_b_min_mid_strikes);
    std::array<std::uint32_t, 4> grades{};
    for (const auto &slice : surface.slices) ++grades[static_cast<std::size_t>(slice.grade)];
    EXPECT_EQ(s->grade_counts, grades);
    auto decoded = decode_surface(*a, *s);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->size(), surface.slices.size());
    for (std::size_t i = 0; i < decoded->size(); ++i) {
      EXPECT_EQ(diff_slice((*decoded)[i], surface.slices[i]), "") << "expiry " << i;
      const FitArchiveKey key{*id, surface.expiry_days[i], kSnapshot, 0, 0};
      EXPECT_EQ(a->find_fit(key), &a->fits_of(*s)[i]);
    }
  }
}

TEST(FitArchiveRecords, EveryCurveKindAndEveryOptionalFieldRoundTrips) {
  const auto surface = testkit::synthetic_surface();
  std::vector<QuoteSurfaceSlice> slices;
  std::vector<std::int32_t> days;
  for (int kind = 0; kind <= 5; ++kind) {
    slices.push_back(testkit::exotic_slice(surface.slices[1], static_cast<VolCurveKind>(kind)));
    days.push_back(21000 + kind);
  }
  FitArchiveWriter w;
  w.record_timings(true);
  ASSERT_TRUE(w.add_surface({"SPY", kSnapshot, slices, days, {}, {}, "dense", "symbol"}).has_value());
  const auto image = w.finish();
  ASSERT_TRUE(image.has_value());
  const FitArchive a = open_image(*image);
  ASSERT_EQ(a.fits().size(), slices.size());
  for (std::size_t i = 0; i < slices.size(); ++i) {
    auto decoded = decode_surface_slice(a, a.fits()[i]);
    ASSERT_TRUE(decoded.has_value()) << to_string(decoded.error().status);
    EXPECT_EQ(diff_slice(*decoded, slices[i]), "") << to_string(static_cast<VolCurveKind>(i));
    // Zero-copy views agree with the owned object.
    const auto nodes = price_nodes(a, a.fits()[i]);
    ASSERT_EQ(nodes.size(), slices[i].fit.prices->nodes().size());
    EXPECT_EQ(std::memcmp(nodes.data(), slices[i].fit.prices->nodes().data(), nodes.size_bytes()), 0);
    EXPECT_EQ(a.moved_nodes(a.fits()[i]).size(), slices[i].fit.moved_nodes.size());
    EXPECT_EQ(fit_dividends(a, a.fits()[i]).size(), 2u);
    EXPECT_EQ(fit_carry_curve(a, a.fits()[i]).size(), 3u);
  }
}

// Every DiscreteDivScheme a fit can record round-trips (review-D1 M2). The open-side
// range check stopped at EscrowedLeisenReimer, so a keys-ON fit-archive-bench run with
// `dividend_lattice: "spliced-cubic"` (lane D1's SplicedCubic, 5) published an archive
// every reader refused (RecordInvalid). The schemes are enumerated by name, so a new
// enumerator with a name joins this test without an edit here.
TEST(FitArchiveRecords, EveryDiscreteDivSchemeRoundTrips) {
  const auto surface = testkit::synthetic_surface();
  std::vector<QuoteSurfaceSlice> slices;
  std::vector<std::int32_t> days;
  for (int v = 0; v <= 255; ++v) {
    const auto scheme = static_cast<DiscreteDivScheme>(v);
    if (std::string(to_string(scheme)) == "unknown") {
      break;
    }
    slices.push_back(testkit::exotic_slice(surface.slices[1], VolCurveKind::ConvexDense));
    slices.back().fit.discrete_div_scheme = scheme;
    days.push_back(21000 + v);
  }
  ASSERT_GE(slices.size(), 6u); // SplicedCrr .. SplicedCubic
  EXPECT_EQ(slices.back().fit.discrete_div_scheme, DiscreteDivScheme::SplicedCubic);
  FitArchiveWriter w;
  w.record_timings(true);
  ASSERT_TRUE(w.add_surface({"SPY", kSnapshot, slices, days, {}, {}, "dense", "symbol"}).has_value());
  const auto image = w.finish();
  ASSERT_TRUE(image.has_value());
  auto a = FitArchive::open_copy(*image, FitArchiveVerify::Paranoid);
  ASSERT_TRUE(a.has_value()) << to_string(a.error().status);
  ASSERT_EQ(a->fits().size(), slices.size());
  for (std::size_t i = 0; i < slices.size(); ++i) {
    auto decoded = decode_surface_slice(*a, a->fits()[i]);
    ASSERT_TRUE(decoded.has_value()) << to_string(decoded.error().status);
    EXPECT_EQ(decoded->fit.discrete_div_scheme, slices[i].fit.discrete_div_scheme);
    EXPECT_EQ(diff_slice(*decoded, slices[i]), "") << to_string(slices[i].fit.discrete_div_scheme);
  }
}

TEST(FitArchiveRecords, StandaloneFitDecodesWithDefaultSliceFields) {
  const auto surface = testkit::synthetic_surface();
  const QuoteSurfaceSlice exotic = testkit::exotic_slice(surface.slices[0], VolCurveKind::SplineVol);
  FitArchiveWriter w;
  w.record_timings(true);
  ASSERT_TRUE(w.add_fit({"TSLA", kSnapshot, 20800, 3, &exotic.fit}).has_value());
  const FitArchive a = open_image(*w.finish());
  ASSERT_EQ(a.fits().size(), 1u);
  EXPECT_EQ(a.fits()[0].surface, kFitArchiveNone);
  auto fit = decode_surface_slice(a, a.fits()[0]);
  ASSERT_TRUE(fit.has_value());
  QuoteSurfaceSlice expected;
  expected.fit = std::move(*decode_fit(a, a.fits()[0]));
  EXPECT_EQ(diff_slice(*fit, expected), "");
  testkit::Diff d;
  testkit::diff_fit(d, fit->fit, exotic.fit);
  EXPECT_EQ(d.first(), "");
  EXPECT_EQ(fit->grade, QuoteSliceGrade::Refused);
}

// A slice refused after a repair is stripped (quote_surface.cpp strip resets
// prices but keeps moved_nodes): its flags must archive and return unchanged
// with no prices (15 dev200 graded-v1 surfaces carry such a slice).
TEST(FitArchiveRecords, StrippedRepairedSliceKeepsMovedNodesWithoutPrices) {
  const auto surface = testkit::synthetic_surface();
  const auto stripped = [&] {
    QuoteSurfaceSlice s = testkit::exotic_slice(surface.slices[1], VolCurveKind::ConvexDense);
    s.fit.prices.reset();
    s.grade = QuoteSliceGrade::Refused;
    return s;
  };
  const QuoteSurfaceSlice expected = stripped();
  ASSERT_FALSE(expected.fit.moved_nodes.empty());
  std::vector<QuoteSurfaceSlice> slices;
  slices.push_back(testkit::exotic_slice(surface.slices[0], VolCurveKind::Essvi));
  slices.push_back(stripped());
  const std::vector<std::int32_t> days{20770, 20833};
  FitArchiveWriter w;
  w.record_timings(true);
  ASSERT_TRUE(
      w.add_surface({"META", kSnapshot, slices, days, surface.config, digest(), "ordinary", "raw-liquidity"})
          .has_value());
  const auto image = w.finish();
  ASSERT_TRUE(image.has_value());
  const FitArchive a = open_image(*image);
  ASSERT_EQ(a.fits().size(), 2u);
  const FitArchiveFitRecord &r = a.fits()[1];
  EXPECT_EQ(r.prices.present, 0u);
  EXPECT_EQ(a.moved_nodes(r).size(), expected.fit.moved_nodes.size());
  auto decoded = decode_surface_slice(a, r);
  ASSERT_TRUE(decoded.has_value()) << to_string(decoded.error().status);
  EXPECT_EQ(diff_slice(*decoded, expected), "");
  EXPECT_FALSE(decoded->fit.prices.has_value());
}

// Lane C0: price-only node sides, typed rows and snapshot provenance archive and return
// bit-exact (the archive dropped QuoteSliceFit::price_only_nodes, so a decoded fit's query
// re-implied a volatility for a side the fit served price-only; and a PriceOnly row made
// the archive unreadable).
TEST(FitArchiveRecords, PriceOnlyNodesAndSnapshotProvenanceSurviveTheArchive) {
  const auto surface = testkit::synthetic_surface();
  const QuoteSurfaceSlice exotic = testkit::exotic_slice(surface.slices[1], VolCurveKind::Essvi);
  ASSERT_TRUE(exotic.fit.prices);
  ASSERT_EQ(exotic.fit.rows[0].evaluation_status, QuoteSliceEvaluationStatus::PriceOnly);
  FitArchiveWriter w;
  w.record_timings(true); // bit-exact including the exotic fit's wall-clock solve time
  ASSERT_TRUE(w.add_fit({"SPY", kSnapshot, 20800, 0, &exotic.fit}).has_value());
  const FitArchive a = open_image(*w.finish());
  ASSERT_EQ(a.fits().size(), 1u);
  const FitArchiveFitRecord &r = a.fits()[0];
  EXPECT_EQ(a.price_only_nodes(r).size(), exotic.fit.price_only_nodes.size());
  EXPECT_EQ(r.floor_policy, 1u);
  EXPECT_EQ(r.spot_version, 2u);
  EXPECT_EQ(r.spot_rule, 1u); // QuoteSliceSpotRule::Nearest
  EXPECT_EQ(r.snapshot_flags, kFitSnapshotSpotStale | kFitSnapshotHasRawSpot);
  EXPECT_EQ(a.quote_rows(r)[0].flags, kQuoteRowHasInterval);
  auto fit = decode_fit(a, r);
  ASSERT_TRUE(fit.has_value()) << to_string(fit.error().status);
  testkit::Diff d;
  testkit::diff_fit(d, *fit, exotic.fit);
  EXPECT_EQ(d.first(), "");
  const auto nodes = exotic.fit.prices->nodes();
  for (std::size_t k = 0; k < nodes.size(); ++k) {
    if ((exotic.fit.price_only_nodes[k] & 2u) == 0u) continue;
    const auto answer = query_quote_slice(*fit, nodes[k].strike, Side::Put);
    ASSERT_TRUE(answer) << k << ": " << answer.error().to_string();
    EXPECT_EQ(answer->status, QuoteSliceEvaluationStatus::PriceOnly) << k;
    EXPECT_FALSE(evaluate_quote_slice(*fit, nodes[k].strike, Side::Put)) << k;
  }
}

TEST(FitArchiveRecords, WritesAreDeterministicAndCopyIsByteIdentical) {
  const auto surface = testkit::synthetic_surface();
  const auto exotic = testkit::exotic_slice(surface.slices[2], VolCurveKind::C8);
  const auto build = [&] {
    FitArchiveWriter w({42, 9, 0});
    EXPECT_TRUE(w.add_surface({"AAPL", kSnapshot, surface.slices, surface.expiry_days, surface.config,
                               digest(), "ordinary", "raw-liquidity"})
                    .has_value());
    EXPECT_TRUE(w.add_fit({"ZM", kSnapshot + 1, 20900, 0, &exotic.fit}).has_value());
    return *w.finish();
  };
  const auto first = build();
  const auto second = build();
  EXPECT_EQ(first, second);
  const FitArchive a = open_image(first);
  FitArchiveWriter copy({42, 9, 0});
  ASSERT_TRUE(copy.copy_from(a).has_value());
  EXPECT_EQ(*copy.finish(), first) << "copy_from must be bit-exact";
}

// Two fits of the same inputs differ only in wall-clock timings (review NIT:
// solve_seconds sat in the checksummed fit record, so archive sha256s could not
// pin parity). By default the archives are byte-identical and timings decode
// as 0; record_timings keeps them, bit-exact, in the optional FitTimings section.
TEST(FitArchiveRecords, ArchivesOfTheSameInputsAreByteIdenticalWhateverTheTimings) {
  const auto surface = testkit::synthetic_surface();
  const auto slices_with = [&](double solve_seconds) {
    std::vector<QuoteSurfaceSlice> slices;
    for (std::size_t i = 0; i < surface.slices.size(); ++i) {
      slices.push_back(testkit::exotic_slice(surface.slices[i], static_cast<VolCurveKind>(i)));
      slices.back().fit.diagnostics.channel.solve_seconds = solve_seconds + static_cast<double>(i);
    }
    return slices;
  };
  const auto write = [&](const std::vector<QuoteSurfaceSlice> &slices, bool timings) {
    FitArchiveWriter w({42, 9, 0});
    w.record_timings(timings);
    EXPECT_TRUE(w.add_surface({"AAPL", kSnapshot, slices, surface.expiry_days, surface.config, digest(),
                               "ordinary", "raw-liquidity"})
                    .has_value());
    EXPECT_TRUE(w.add_fit({"ZM", kSnapshot, 20900, 0, &slices[0].fit}).has_value());
    return *w.finish();
  };
  const auto run1 = slices_with(0.25);
  const auto run2 = slices_with(0.75);
  ASSERT_FALSE(bits(run1[0].fit.diagnostics.channel.solve_seconds, run2[0].fit.diagnostics.channel.solve_seconds));

  // Default: byte-identical, no FitTimings, timings decode as 0, the rest bit-exact.
  const auto a1 = write(run1, false);
  const auto a2 = write(run2, false);
  EXPECT_EQ(a1, a2) << "same inputs, different wall-clock timings: bytes must not differ";
  const FitArchive a = open_image(a1);
  EXPECT_EQ(a.section(FitArchiveSectionType::FitTimings), nullptr);
  ASSERT_EQ(a.fits().size(), run1.size() + 1);
  for (std::size_t i = 0; i < run1.size(); ++i) {
    auto decoded = decode_surface_slice(a, a.fits()[i]);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(bits(decoded->fit.diagnostics.channel.solve_seconds, 0.0));
    QuoteSurfaceSlice expected = testkit::exotic_slice(surface.slices[i], static_cast<VolCurveKind>(i));
    expected.fit.diagnostics.channel.solve_seconds = 0.0;
    EXPECT_EQ(diff_slice(*decoded, expected), "") << i;
  }

  // Opt-in: timings archived (so the bytes differ) and decoded bit-exactly.
  const auto t1 = write(run1, true);
  const auto t2 = write(run2, true);
  EXPECT_NE(t1, t2);
  const FitArchive t = open_image(t1);
  ASSERT_NE(t.section(FitArchiveSectionType::FitTimings), nullptr);
  for (std::size_t i = 0; i < run1.size(); ++i) {
    auto decoded = decode_surface_slice(t, t.fits()[i]);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(diff_slice(*decoded, run1[i]), "") << i;
  }
  // A record copy is not an element of the archive: no timing is attributed.
  const FitArchiveFitRecord detached = t.fits()[0];
  auto loose = decode_fit(t, detached);
  ASSERT_TRUE(loose.has_value());
  EXPECT_TRUE(bits(loose->diagnostics.channel.solve_seconds, 0.0));
  // copy_from keeps a source's timings, byte for byte.
  FitArchiveWriter copy({42, 9, 0});
  ASSERT_TRUE(copy.copy_from(t).has_value());
  EXPECT_EQ(*copy.finish(), t1);
}

ExpiryRecord build_record(Side side, double expiry, bool certify) {
  ExpiryRecordSpec spec;
  spec.expiry = expiry;
  spec.carry = {0.04, 0.01, {}};
  spec.side = side;
  spec.k_min = -0.35;
  spec.k_max = 0.3;
  spec.sigma_min = 0.1;
  spec.sigma_max = 0.6;
  spec.certify = certify;
  auto r = ExpiryRecord::build(spec);
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  return r ? *r : ExpiryRecord{};
}

TEST(FitArchiveRecords, ExpiryRecordsServeZeroCopyAndBitIdentical) {
  const ExpiryRecord put = build_record(Side::Put, 0.3, true);
  const ExpiryRecord call = build_record(Side::Call, 0.3, false);
  FitArchiveWriter w;
  ASSERT_TRUE(w.add_expiry_record({"IBM", kSnapshot, 20850, 0, &put}).has_value());
  ASSERT_TRUE(w.add_expiry_record({"IBM", kSnapshot, 20850, 0, &call}).has_value());
  const FitArchive a = open_image(*w.finish());
  const auto records = expiry_records(a);
  ASSERT_EQ(records.size(), 2u);
  const auto id = a.find_underlier("IBM");
  ASSERT_TRUE(id.has_value());
  for (const ExpiryRecord *original : {&put, &call}) {
    const FitArchiveKey key{*id, 20850, kSnapshot, static_cast<std::uint32_t>(original->side()) << 16, 0};
    const FitArchiveExpiryMeta *meta = a.find_expiry(key);
    ASSERT_NE(meta, nullptr);
    const ExpiryRecord *stored = expiry_record(a, *meta);
    ASSERT_NE(stored, nullptr);
    EXPECT_EQ(std::memcmp(stored, original, kFitArchiveExpiryRecordValueBytes), 0);
    EXPECT_TRUE(bits(meta->certified_max_abs_error_fwd, original->certified_max_abs_error_fwd()));
    for (const double K : {80.0, 95.0, 100.0, 104.0, 120.0}) {
      for (const double sigma : {0.15, 0.27, 0.45}) {
        EXPECT_TRUE(bits(stored->price(100.0, K, sigma), original->price(100.0, K, sigma)));
        const RecordGreeks g = stored->greeks(101.0, K, sigma);
        const RecordGreeks h = original->greeks(101.0, K, sigma);
        EXPECT_TRUE(bits(g.price, h.price) && bits(g.delta, h.delta) && bits(g.gamma, h.gamma) &&
                    bits(g.vega, h.vega) && bits(g.theta, h.theta) && g.exercised == h.exercised);
      }
    }
  }
}

struct Chain {
  std::vector<double> strike, expiry, sigma, rate, carry;
  std::vector<Side> side;
  [[nodiscard]] ChainContractsView view() const { return {strike, expiry, sigma, rate, carry, side}; }
};

Chain small_chain() {
  Chain c;
  for (const double T : {0.15, 0.4}) {
    for (const Side s : {Side::Call, Side::Put}) {
      for (const double K : {90.0, 100.0, 110.0}) {
        c.strike.push_back(K);
        c.expiry.push_back(T);
        c.sigma.push_back(0.25 + 0.1 * std::log(100.0 / K));
        c.rate.push_back(0.04);
        c.carry.push_back(0.01);
        c.side.push_back(s);
      }
    }
  }
  return c;
}

void expect_same_bank(const ExpiryRecordBank &x, const ExpiryRecordBank &y) {
  ASSERT_EQ(x.size(), y.size());
  for (std::size_t i = 0; i < x.size(); ++i) {
    EXPECT_EQ(std::memcmp(x.at(i), y.at(i), kFitArchiveExpiryRecordValueBytes), 0) << "record " << i;
  }
}

TEST(FitArchiveRecords, ChainRecordsRoundTripAndRebuildTheSameLadder) {
  const Chain chain = small_chain();
  constexpr double kSpot = 100.0;
  ChainRecordPolicy record_policy;
  SpotLadderPolicy ladder_policy;
  ladder_policy.rebuild_order = SpotLadderRebuildOrder::NearestAtmFirst;
  auto records = ChainRecords::build(kSpot, chain.view(), record_policy);
  ASSERT_TRUE(records.has_value()) << records.error().to_string();
  const std::vector<std::int32_t> days(records->bank().size(), 20900);
  FitArchiveWriter w;
  ASSERT_TRUE(w.add_chain_records({"KO", kSnapshot, 0, 0, 0, kSpot, chain.view(), &*records, record_policy,
                                   ladder_policy, days})
                  .has_value());
  const FitArchive a = open_image(*w.finish());
  ASSERT_EQ(a.ladders().size(), 1u);
  const FitArchiveLadderRecord &l = a.ladders()[0];
  auto source = decode_chain_source(a, l);
  ASSERT_TRUE(source.has_value());
  EXPECT_TRUE(std::equal(source->record_of_contract.begin(), source->record_of_contract.end(),
                         records->record_of_contract().begin(), records->record_of_contract().end()));
  EXPECT_EQ(source->ladder_policy.rebuild_order, SpotLadderRebuildOrder::NearestAtmFirst);
  EXPECT_TRUE(bits(source->worst_certified_error_fwd, records->worst_certified_error_fwd()));
  auto bank = decode_expiry_bank(a, l);
  ASSERT_TRUE(bank.has_value());
  expect_same_bank(*bank, records->bank());
  // The stored chain + policy rebuild the identical records and ladder.
  auto rebuilt = ChainRecords::build(source->spot, source->view(), source->record_policy);
  ASSERT_TRUE(rebuilt.has_value());
  expect_same_bank(rebuilt->bank(), records->bank());
  auto ladder_a = ChainSpotLadder::build(kSpot, chain.view(), *records, ladder_policy);
  auto ladder_b = ChainSpotLadder::build(source->spot, source->view(), *rebuilt, source->ladder_policy);
  ASSERT_TRUE(ladder_a.has_value() && ladder_b.has_value());
  const std::size_t n = chain.strike.size();
  std::vector<double> pa(n), da(n), ga(n), va(n), ta(n), pb(n), db(n), gb(n), vb(n), tb(n);
  std::vector<TickStatus> sa(n), sb(n);
  ASSERT_TRUE(ladder_a->refresh(100.4, {pa, da, ga, va, ta, sa}).has_value());
  ASSERT_TRUE(ladder_b->refresh(100.4, {pb, db, gb, vb, tb, sb}).has_value());
  EXPECT_TRUE(std::equal(pa.begin(), pa.end(), pb.begin(), bits) && std::equal(da.begin(), da.end(), db.begin(), bits) &&
              std::equal(ga.begin(), ga.end(), gb.begin(), bits) && sa == sb);
  // Serving straight from the archive: each contract's stored record.
  const auto metas = a.ladder_records(l);
  for (std::size_t i = 0; i < n; ++i) {
    const ExpiryRecord *rec = expiry_record(a, metas[source->record_of_contract[i]]);
    const ExpiryRecord *ref = records->bank().at(records->record_of_contract()[i]);
    ASSERT_NE(rec, nullptr);
    EXPECT_TRUE(bits(rec->price(100.4, chain.strike[i], chain.sigma[i]),
                     ref->price(100.4, chain.strike[i], chain.sigma[i])));
  }
}

struct MarkovFixture {
  std::vector<double> grid;
  std::vector<AmericanMarkovLocalVolNode> nodes{{-0.4, 1.15}, {0.0, 1.0}, {0.35, 0.92}};
  std::vector<AmericanMarkovInterval> intervals;
  AmericanMarkovSpec spec;
  MarkovFixture() {
    for (int i = 0; i <= 40; ++i) grid.push_back(10.0 * i);
    intervals = {{0.25, 0.24, 0.035, 0.01, 0.27, 0.0, nodes}, {0.5, 0.49, 0.035, 0.01, 0.23, 0.8, {}}};
    spec = {100.0, intervals, grid, {}};
    spec.config.max_calendar_step = 1.0 / 104.0;
  }
};

void expect_same_model(const AmericanMarkovModel &x, const AmericanMarkovModel &y) {
  ASSERT_EQ(x.layers().size(), y.layers().size());
  for (std::size_t i = 0; i < x.layers().size(); ++i) {
    const auto &p = x.layers()[i];
    const auto &q = y.layers()[i];
    EXPECT_TRUE(bits(p.calendar_time, q.calendar_time) && bits(p.variance_time, q.variance_time) &&
                bits(p.growth, q.growth));
    EXPECT_TRUE(std::equal(p.states().begin(), p.states().end(), q.states().begin(), q.states().end(), bits));
  }
  ASSERT_EQ(x.transitions().size(), y.transitions().size());
  for (std::size_t i = 0; i < x.transitions().size(); ++i) {
    const auto &p = x.transitions()[i];
    const auto &q = y.transitions()[i];
    EXPECT_TRUE(p.kind == q.kind && bits(p.discount, q.discount) && bits(p.variance_step, q.variance_step) &&
                p.interval_index == q.interval_index);
    ASSERT_EQ(p.rows().size(), q.rows().size());
    for (std::size_t r = 0; r < p.rows().size(); ++r) {
      EXPECT_EQ(p.rows()[r].destination, q.rows()[r].destination);
      EXPECT_TRUE(std::equal(p.rows()[r].probability.begin(), p.rows()[r].probability.end(),
                             q.rows()[r].probability.begin(), bits));
    }
  }
  EXPECT_TRUE(std::equal(x.expiry_layers().begin(), x.expiry_layers().end(), y.expiry_layers().begin(),
                         y.expiry_layers().end()));
}

TEST(FitArchiveRecords, MarkovSpecRoundTripCompilesTheSameModel) {
  const MarkovFixture fx;
  auto original = compile_american_markov(fx.spec);
  ASSERT_TRUE(original.has_value()) << original.error().to_string();
  // A small fit, for the optional summary arrays.
  const std::vector<AmericanMarkovFitInterval> fit_intervals{{0.25, 0.24, 0.035, 0.01, 0.0},
                                                             {0.5, 0.49, 0.035, 0.01, 0.8}};
  const AmericanMarkovFitMarket market{100.0, fit_intervals, fx.grid, fx.spec.config};
  std::vector<AmericanMarkovFitQuote> quotes;
  AmericanMarkovWorkspace ws;
  ASSERT_TRUE(ws.prepare(*original).has_value());
  for (std::size_t e = 0; e < 2; ++e) {
    for (const double K : {90.0, 100.0, 110.0}) {
      const auto p = price_american_markov(*original, K, Side::Put, e, ws);
      ASSERT_TRUE(p.has_value());
      quotes.push_back({e, K, Side::Put, std::max(*p - 0.05, 0.0), *p + 0.05, 1.0});
    }
  }
  AmericanMarkovFitConfig fit_config;
  fit_config.max_evaluations = 6;
  auto fit = fit_american_markov_intervals(market, quotes, fit_config);
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();

  FitArchiveWriter w;
  ASSERT_TRUE(w.add_markov({"SPY", kSnapshot, 0, &fx.spec, &*fit, "common-r104"}).has_value());
  ASSERT_TRUE(w.add_markov({"SPY", kSnapshot, 1, &fx.spec, nullptr, ""}).has_value());
  const FitArchive a = open_image(*w.finish());
  ASSERT_EQ(a.markov().size(), 2u);
  const auto id = a.find_underlier("SPY");
  const FitArchiveMarkovRecord *r = a.find_markov({*id, 0, kSnapshot, 0, 0});
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(a.string(r->label), "common-r104");
  const auto sigmas = a.markov_words(r->sigmas);
  EXPECT_TRUE(std::equal(sigmas.begin(), sigmas.end(), fit->sigmas().begin(), fit->sigmas().end(), bits));
  const auto prices = a.markov_words(r->quote_prices);
  EXPECT_TRUE(std::equal(prices.begin(), prices.end(), fit->quote_prices().begin(), fit->quote_prices().end(), bits));
  auto spec = decode_markov_spec(a, *r);
  ASSERT_TRUE(spec.has_value());
  auto again = compile_american_markov(spec->spec());
  ASSERT_TRUE(again.has_value());
  expect_same_model(*original, *again);
  AmericanMarkovWorkspace w2;
  ASSERT_TRUE(w2.prepare(*again).has_value());
  for (std::size_t e = 0; e < 2; ++e) {
    const auto p = price_american_markov(*original, 97.5, Side::Put, e, ws);
    const auto q = price_american_markov(*again, 97.5, Side::Put, e, w2);
    ASSERT_TRUE(p && q);
    EXPECT_TRUE(bits(*p, *q));
  }
  EXPECT_EQ(a.find_markov({*id, 0, kSnapshot, 1, 0})->has_fit, 0u);
}

TEST(FitArchiveRecords, OpenedArchiveServesWithoutAllocating) {
  const auto surface = testkit::synthetic_surface();
  const ExpiryRecord put = build_record(Side::Put, 0.3, false);
  FitArchiveWriter w;
  ASSERT_TRUE(w.add_surface({"AAPL", kSnapshot, surface.slices, surface.expiry_days, surface.config, digest(),
                             "ordinary", "raw-liquidity"})
                  .has_value());
  ASSERT_TRUE(w.add_expiry_record({"AAPL", kSnapshot, 20833, 0, &put}).has_value());
  const auto dir = testkit::scratch_dir("alloc");
  const auto path = (dir / "a.atxvfa").string();
  ASSERT_TRUE(w.write_file(path).has_value());
  auto a = FitArchive::open(path);
  ASSERT_TRUE(a.has_value());
  double sink = 0.0;
  std::size_t found = 0;
  const std::uint64_t allocations = testkit::count_allocations([&] {
    for (int rep = 0; rep < 100; ++rep) {
      const auto id = a->find_underlier("AAPL");
      const FitArchiveSurfaceRecord *s = a->find_surface(*id, kSnapshot);
      for (const FitArchiveFitRecord &f : a->fits_of(*s)) {
        const FitArchiveFitRecord *hit = a->find_fit(f.key);
        found += hit == &f ? 1u : 0u;
        for (const auto &row : a->quote_rows(f)) sink += row.fitted_price;
        for (const auto &node : price_nodes(*a, f)) sink += node.call;
        sink += static_cast<double>(a->curve_payload(f).size() + a->string(f.carry.reason).size());
      }
      const FitArchiveExpiryMeta *m = a->find_expiry({*id, 20833, kSnapshot, 1u << 16, 0});
      const ExpiryRecord *rec = expiry_record(*a, *m);
      sink += rec->price(100.0, 95.0, 0.3) + rec->greeks(100.0, 105.0, 0.25).delta;
      sink += static_cast<double>(a->underlier_name(*id).size());
    }
  });
  EXPECT_EQ(allocations, 0u);
  EXPECT_EQ(found, 300u);
  EXPECT_TRUE(std::isfinite(sink));
}

TEST(FitArchiveRecordRefusal, CorruptExpiryRecordBytes) {
  const ExpiryRecord put = build_record(Side::Put, 0.3, false);
  FitArchiveWriter w;
  ASSERT_TRUE(w.add_expiry_record({"IBM", kSnapshot, 20850, 0, &put}).has_value());
  const auto image = *w.finish();
  EXPECT_EQ(open_status(image), ArchiveStatus::Ok);
  const auto mutate = [&](auto fn) {
    auto sections = split_image(image);
    fn(*find_section(sections, Type::ExpiryData));
    return open_status(join_image(sections));
  };
  // A bool byte that is not 0/1, tail padding, and a member the meta disagrees with.
  EXPECT_EQ(mutate([](testkit::ImageSection &s) { s.bytes[1817] = std::byte{2}; }), ArchiveStatus::LayoutMismatch);
  EXPECT_EQ(mutate([](testkit::ImageSection &s) { s.bytes[1822] = std::byte{1}; }), ArchiveStatus::LayoutMismatch);
  EXPECT_EQ(mutate([](testkit::ImageSection &s) { s.bytes[1536 + 192] ^= std::byte{4}; }), ArchiveStatus::LayoutMismatch);
  EXPECT_EQ(mutate([](testkit::ImageSection &s) {
              s.bytes.resize(s.bytes.size() + 8);
              s.meta.element_stride += 8;
            }),
            ArchiveStatus::LayoutMismatch);
  auto meta = split_image(image);
  auto *m = find_section(meta, Type::ExpiryMeta);
  m->bytes[offsetof(FitArchiveExpiryMeta, populated)] = std::byte{0};
  EXPECT_EQ(open_status(join_image(meta)), ArchiveStatus::RecordInvalid);
}

TEST(FitArchiveRecordRefusal, CorruptCurvePayloadAndUncertifiablePrices) {
  const auto surface = testkit::synthetic_surface();
  std::vector<QuoteSurfaceSlice> slices;
  slices.push_back(testkit::exotic_slice(surface.slices[1], VolCurveKind::C8));
  slices.push_back(testkit::exotic_slice(surface.slices[1], VolCurveKind::Essvi));
  const std::vector<std::int32_t> days{21000, 21001};
  FitArchiveWriter w;
  ASSERT_TRUE(w.add_surface({"SPY", kSnapshot, slices, days}).has_value());
  const auto image = *w.finish();
  const auto mutate_words = [&](std::size_t fit, std::size_t byte, std::byte value) {
    auto sections = split_image(image);
    const auto r = testkit::read_at<FitArchiveFitRecord>(find_section(sections, Type::FitRecords)->bytes,
                                                         fit * sizeof(FitArchiveFitRecord));
    find_section(sections, Type::CurveWords)->bytes[r.curve.payload.first * 8u + byte] = value;
    return open_status(join_image(sections));
  };
  EXPECT_EQ(mutate_words(0, offsetof(C8Params, bumps_active), std::byte{2}), ArchiveStatus::RecordInvalid);
  EXPECT_EQ(mutate_words(1, offsetof(EssviParams, resid_basis_kind), std::byte{9}), ArchiveStatus::RecordInvalid);
  // Structurally valid, but a node price that breaks convexity cannot re-certify.
  auto sections = split_image(image);
  auto *nodes = find_section(sections, Type::PriceNodes);
  testkit::write_at(nodes->bytes, sizeof(AmericanPriceNode) * 3 + offsetof(AmericanPriceNode, call), 1.0e6);
  const auto broken = join_image(sections);
  const FitArchive a = open_image(broken);
  auto decoded = decode_fit(a, a.fits()[0]);
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().status, ArchiveStatus::RecordInvalid);
}

// One fit of every curve kind (enum order, so fit i has kind i), for the
// node-count overflow refusals (review MAJOR-1).
std::vector<std::byte> every_kind_image() {
  const auto surface = testkit::synthetic_surface();
  std::vector<QuoteSurfaceSlice> slices;
  std::vector<std::int32_t> days;
  for (int kind = 0; kind <= 5; ++kind) {
    slices.push_back(testkit::exotic_slice(surface.slices[1], static_cast<VolCurveKind>(kind)));
    days.push_back(21000 + kind);
  }
  FitArchiveWriter w;
  EXPECT_TRUE(w.add_surface({"SPY", kSnapshot, slices, days, {}, {}, "dense", "symbol"}).has_value());
  auto image = w.finish();
  EXPECT_TRUE(image.has_value());
  return image ? std::move(*image) : std::vector<std::byte>{};
}

// The count whose doubled value wraps a u32 back to the stored payload length:
// n + 2^31 for a kind with nodes, 2^31 for a fixed-size kind (whose count is 0).
std::uint32_t wrapping_count(const FitArchiveCurve &c) { return c.node_count + 0x80000000u; }

// A curve node_count whose 2n wraps a u32 passed the 32-bit word-count check, so
// open accepted the file and the decoders read ~16 GiB past the payload. Every
// such count, for every curve kind, is refused at every verify level by the
// record pass of open, before any view or decoder can reach the payload.
TEST(FitArchiveRecordRefusal, CurveNodeCountThatWrapsU32IsRefusedForEveryCurveKind) {
  const auto image = every_kind_image();
  const FitArchive a = open_image(image);
  ASSERT_EQ(a.fits().size(), 6u);
  const auto files = testkit::scratch_dir("node-count-wraps-u32");
  for (std::size_t i = 0; i < a.fits().size(); ++i) {
    const FitArchiveCurve &c = a.fits()[i].curve;
    ASSERT_EQ(static_cast<std::size_t>(c.kind), i);
    const std::string kind = to_string(static_cast<VolCurveKind>(c.kind));
    for (const std::uint32_t count : {wrapping_count(c), 0x80000000u, 0xFFFFFFFFu}) {
      const auto forged = testkit::forge_node_count(image, i, count);
      ASSERT_FALSE(forged.empty());
      for (const auto verify : {FitArchiveVerify::Framing, FitArchiveVerify::Full, FitArchiveVerify::Paranoid}) {
        auto opened = FitArchive::open_copy(forged, verify);
        ASSERT_FALSE(opened.has_value())
            << kind << " node_count " << count << " verify " << static_cast<int>(verify);
        EXPECT_EQ(opened.error().status, ArchiveStatus::RecordInvalid) << kind << " " << count;
        EXPECT_EQ(opened.error().section, static_cast<std::uint32_t>(Type::FitRecords)) << kind;
        EXPECT_EQ(opened.error().detail, i) << kind;
      }
    }
    // The wrapping case as a file, for the CLI and the crafted-file table.
    const auto file = files / ("node-count-wraps-u32-" + kind + ".atxvfa");
    const auto wrapped = testkit::forge_node_count(image, i, wrapping_count(c));
    ASSERT_TRUE(publish_fit_archive(file.string(), wrapped).has_value());
    auto from_disk = FitArchive::open(file.string(), {FitArchiveOpenMode::Mapped, FitArchiveVerify::Paranoid});
    ASSERT_FALSE(from_disk.has_value()) << kind;
    EXPECT_EQ(from_disk.error().status, ArchiveStatus::RecordInvalid) << kind;
  }
}

// The decoders also guard record copies a caller edited (open cannot validate
// those): a wrapping count is refused before any payload word is read. Only run
// with the fix in place: with 32-bit arithmetic this test reads out of bounds.
TEST(FitArchiveRecordRefusal, DecodersRefuseARecordCopyWhoseNodeCountWraps) {
  const auto image = every_kind_image();
  const FitArchive a = open_image(image);
  ASSERT_EQ(a.fits().size(), 6u);
  for (std::size_t i = 0; i < a.fits().size(); ++i) {
    ASSERT_TRUE(decode_curve(a, a.fits()[i]).has_value()) << i;
    for (const std::uint32_t count : {wrapping_count(a.fits()[i].curve), 0xFFFFFFFFu}) {
      FitArchiveFitRecord copy = a.fits()[i];
      copy.curve.node_count = count;
      const std::string kind = to_string(static_cast<VolCurveKind>(copy.curve.kind));
      auto curve = decode_curve(a, copy);
      ASSERT_FALSE(curve.has_value()) << kind << " " << count;
      EXPECT_EQ(curve.error().status, ArchiveStatus::RecordInvalid) << kind;
      auto fit = decode_fit(a, copy);
      ASSERT_FALSE(fit.has_value()) << kind;
      EXPECT_EQ(fit.error().status, ArchiveStatus::RecordInvalid) << kind;
      auto slice = decode_surface_slice(a, copy);
      ASSERT_FALSE(slice.has_value()) << kind;
      EXPECT_EQ(slice.error().status, ArchiveStatus::RecordInvalid) << kind;
    }
  }
  // decode_surface reads only the archive's own records, which open validated.
  ASSERT_EQ(a.surfaces().size(), 1u);
  auto surface = decode_surface(a, a.surfaces()[0]);
  ASSERT_TRUE(surface.has_value());
  EXPECT_EQ(surface->size(), 6u);
}

} // namespace
} // namespace atx::vol
