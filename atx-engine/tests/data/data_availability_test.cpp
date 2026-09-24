#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/engine/data/adapt_factor.hpp"
#include "atx/engine/data/adapt_panel.hpp"
#include "atx/engine/data/align.hpp"
#include "atx/engine/data/catalog.hpp"
#include "atx/engine/data/dataset.hpp"

namespace atxtest_data_availability {
namespace {

using namespace atx::engine::data;

atx::core::Result<Dataset> make_dataset(DateKeyEncoding encoding, atx::u16 delay,
                                       std::vector<DateKey> dates) {
  DatasetSchema schema;
  schema.columns = {"val"};
  schema.dtypes = {ColumnDType::F64};
  schema.role = Role::Feature;
  schema.pit_delay = delay;
  schema.date_encoding = encoding;
  std::vector<atx::f64> values(dates.size());
  for (atx::usize d = 0; d < dates.size(); ++d) {
    values[d] = static_cast<atx::f64>(d + 1U);
  }
  return Dataset::create(std::move(schema), std::move(dates), {7}, {std::move(values)}, {},
                         {"availability-test", ""});
}

TEST(DataAvailability, OpaqueKeysPreserveZeroDelayAndRejectPositiveDelay) {
  auto legacy = make_dataset(DateKeyEncoding::Opaque, 0, {-3, 10, 20230101});
  ASSERT_TRUE(legacy.has_value());
  EXPECT_EQ(std::vector<DateKey>(legacy->available_dates().begin(),
                                 legacy->available_dates().end()),
            (std::vector<DateKey>{-3, 10, 20230101}));
  EXPECT_FALSE(make_dataset(DateKeyEncoding::Opaque, 1, {10}).has_value());
  EXPECT_FALSE(make_dataset(DateKeyEncoding::Opaque, 1, {}).has_value());
}

TEST(DataAvailability, CalendarDelayCrossesLeapDayMonthAndYear) {
  auto leap = make_dataset(DateKeyEncoding::YYYYMMDD, 2, {20240228, 20241231});
  ASSERT_TRUE(leap.has_value());
  EXPECT_EQ(leap->available_dates()[0], 20240301);
  EXPECT_EQ(leap->available_dates()[1], 20250102);
  auto ordinary = make_dataset(DateKeyEncoding::YYYYMMDD, 1, {20230228});
  ASSERT_TRUE(ordinary.has_value());
  EXPECT_EQ(ordinary->available_dates()[0], 20230301);
}

TEST(DataAvailability, InvalidCalendarDatesAndOverflowFailClosed) {
  for (const DateKey date : {DateKey{20230229}, DateKey{20241301}, DateKey{20240100},
                             DateKey{-1}, DateKey{100000101}}) {
    EXPECT_FALSE(make_dataset(DateKeyEncoding::YYYYMMDD, 0, {date}).has_value());
  }
  EXPECT_FALSE(make_dataset(DateKeyEncoding::YYYYMMDD, 1, {99991231}).has_value());
  const DateKey max_date = std::numeric_limits<DateKey>::max();
  EXPECT_FALSE(make_dataset(DateKeyEncoding::EpochDays, 1, {max_date}).has_value());
  EXPECT_FALSE(make_dataset(DateKeyEncoding::UnixNanoseconds, 1, {max_date}).has_value());
}

TEST(DataAvailability, EpochDaysHandleNegativeKeysAndLargestLag) {
  auto ds = make_dataset(DateKeyEncoding::EpochDays, 65535, {-65536, -65535});
  ASSERT_TRUE(ds.has_value());
  EXPECT_EQ(ds->available_dates()[0], -1);
  EXPECT_EQ(ds->available_dates()[1], 0);
  auto row = ds->available_as_of_index(-1);
  ASSERT_TRUE(row.has_value());
  ASSERT_TRUE(row->has_value());
  EXPECT_EQ(**row, 0U);
}

TEST(DataAvailability, UnixNanosecondsDelayPreservesTimeOfDay) {
  constexpr DateKey day = 86'400'000'000'000;
  constexpr DateKey noon = day / 2;
  auto ds = make_dataset(DateKeyEncoding::UnixNanoseconds, 2, {-noon, noon});
  ASSERT_TRUE(ds.has_value());
  EXPECT_EQ(ds->available_dates()[0], day + noon);
  EXPECT_EQ(ds->available_dates()[1], 2 * day + noon);
  auto before = ds->available_as_of_index(day + noon - 1);
  ASSERT_TRUE(before.has_value());
  EXPECT_FALSE(before->has_value());
  auto at = ds->available_as_of_index(day + noon);
  ASSERT_TRUE(at.has_value());
  ASSERT_TRUE(at->has_value());
  EXPECT_EQ(**at, 0U);
}

TEST(DataAvailability, AlignmentWaitsForAvailabilityAndReportsUnreleasedRows) {
  auto canonical = make_dataset(DateKeyEncoding::YYYYMMDD, 0,
                                 {20240228, 20240229, 20240301, 20240302});
  auto plug = make_dataset(DateKeyEncoding::YYYYMMDD, 2, {20240228, 20240301});
  ASSERT_TRUE(canonical.has_value());
  ASSERT_TRUE(plug.has_value());
  auto aligned = align_onto(*canonical, *plug);
  ASSERT_TRUE(aligned.has_value());
  const auto &values = aligned->aligned_columns[0];
  EXPECT_TRUE(std::isnan(values[0]));
  EXPECT_TRUE(std::isnan(values[1]));
  EXPECT_DOUBLE_EQ(values[2], 1.0);
  EXPECT_DOUBLE_EQ(values[3], 1.0);
  EXPECT_EQ(aligned->drops.extra_date_cells, 1U);
}

TEST(DataAvailability, JoinsRejectMixedEncodings) {
  auto canonical = make_dataset(DateKeyEncoding::EpochDays, 0, {20000});
  auto plug = make_dataset(DateKeyEncoding::YYYYMMDD, 1, {20240101});
  ASSERT_TRUE(canonical.has_value());
  ASSERT_TRUE(plug.has_value());
  EXPECT_FALSE(align_onto(*canonical, *plug).has_value());
}

TEST(DataAvailability, CatalogUsesSameAvailabilityBoundaryAndRejectsInvalidQuery) {
  auto ds = make_dataset(DateKeyEncoding::YYYYMMDD, 2, {20240228, 20240301});
  ASSERT_TRUE(ds.has_value());
  DatasetCatalog catalog;
  ASSERT_TRUE(catalog.register_dataset("feature", std::move(*ds)).has_value());
  auto before = catalog.value_at("feature", "val", 20240229, 7);
  auto at = catalog.value_at("feature", "val", 20240301, 7);
  auto next = catalog.value_at("feature", "val", 20240303, 7);
  ASSERT_TRUE(before.has_value());
  ASSERT_TRUE(at.has_value());
  ASSERT_TRUE(next.has_value());
  EXPECT_TRUE(std::isnan(*before));
  EXPECT_DOUBLE_EQ(*at, 1.0);
  EXPECT_DOUBLE_EQ(*next, 2.0);
  EXPECT_FALSE(catalog.value_at("feature", "val", 20240230, 7).has_value());
}

TEST(DataAvailability, FactorReferenceUsesAvailabilityAndRequiresMatchingEncoding) {
  DatasetSchema schema;
  schema.columns = {"market_cap", "group_id"};
  schema.dtypes = {ColumnDType::F64, ColumnDType::Category};
  schema.role = Role::Reference;
  schema.pit_delay = 2;
  schema.date_encoding = DateKeyEncoding::EpochDays;
  auto ref = Dataset::create(schema, {10}, {7}, {{1000.0}, {3.0}}, {}, {"test", ""});
  auto price = make_dataset(DateKeyEncoding::EpochDays, 0, {10, 11, 12});
  ASSERT_TRUE(ref.has_value());
  ASSERT_TRUE(price.has_value());
  auto before = reference_spans(*ref, *price, 11, 99);
  auto at = reference_spans(*ref, *price, 12, 99);
  ASSERT_TRUE(before.has_value());
  ASSERT_TRUE(at.has_value());
  EXPECT_TRUE(std::isnan(before->market_cap[0]));
  EXPECT_EQ(before->group_id[0], 99U);
  EXPECT_DOUBLE_EQ(at->market_cap[0], 1000.0);
  EXPECT_EQ(at->group_id[0], 3U);
  auto opaque = make_dataset(DateKeyEncoding::Opaque, 0, {10});
  ASSERT_TRUE(opaque.has_value());
  EXPECT_FALSE(reference_spans(*ref, *opaque, 12, 99).has_value());
}

TEST(DataAvailability, LaterSourceRowsCannotChangePastAvailability) {
  auto full = make_dataset(DateKeyEncoding::EpochDays, 3, {10, 20, 30});
  auto prefix = make_dataset(DateKeyEncoding::EpochDays, 3, {10, 20});
  ASSERT_TRUE(full.has_value());
  ASSERT_TRUE(prefix.has_value());
  for (const DateKey decision : {DateKey{9}, DateKey{12}, DateKey{13}, DateKey{22}, DateKey{23}}) {
    auto a = full->available_as_of_index(decision);
    auto b = prefix->available_as_of_index(decision);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(*a, *b);
  }
}

TEST(DataAvailability, PanelBridgeRejectsDelayedRawData) {
  DatasetSchema schema;
  schema.columns = {"close", "volume", "high", "low"};
  schema.dtypes.assign(4, ColumnDType::F64);
  schema.role = Role::Price;
  schema.pit_delay = 2;
  schema.date_encoding = DateKeyEncoding::EpochDays;
  auto delayed = Dataset::create(schema, {10, 11}, {7},
      {{100.0, 101.0}, {1000.0, 1200.0}, {102.0, 103.0}, {98.0, 99.0}}, {}, {"test", ""});
  ASSERT_TRUE(delayed.has_value());
  const auto panel = price_to_panel(*delayed, {});
  ASSERT_FALSE(panel.has_value());
  EXPECT_EQ(panel.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(DataAvailability, RealignedValuesUseZeroLagWhenWrappedInDataset) {
  auto canonical = make_dataset(DateKeyEncoding::EpochDays, 0, {10, 11, 12});
  auto source = make_dataset(DateKeyEncoding::EpochDays, 2, {10});
  ASSERT_TRUE(canonical.has_value());
  ASSERT_TRUE(source.has_value());
  auto aligned = align_onto(*canonical, *source);
  ASSERT_TRUE(aligned.has_value());
  DatasetSchema schema = source->schema();
  schema.pit_delay = 0;
  auto wrapped = Dataset::create(schema, {10, 11, 12}, {7},
                                  std::move(aligned->aligned_columns), {}, {"aligned", ""});
  ASSERT_TRUE(wrapped.has_value());
  DatasetCatalog catalog;
  ASSERT_TRUE(catalog.register_dataset("aligned", std::move(*wrapped)).has_value());
  auto at = catalog.value_at("aligned", "val", 12, 7);
  ASSERT_TRUE(at.has_value());
  EXPECT_DOUBLE_EQ(*at, 1.0);
}

TEST(DataAvailability, PublicLookupRejectsUnsortedAxesWithoutChangingCreateContract) {
  auto ds = make_dataset(DateKeyEncoding::EpochDays, 1, {20, 10});
  ASSERT_TRUE(ds.has_value());
  EXPECT_FALSE(ds->available_as_of_index(30).has_value());
}

} // namespace
} // namespace atxtest_data_availability
