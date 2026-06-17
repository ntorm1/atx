// Tests for DatasetCatalog (S6.2) — TDD suite.
//
// Suite: DataCatalog
// All test names match the spec verbatim so ctest -R DataCatalog picks them up.

#include "atx/engine/data/catalog.hpp"

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/engine/data/dataset.hpp"
#include "atx/engine/data/dataset_schema.hpp"

namespace {

using namespace atx::engine::data;
using atx::core::ErrorCode;

// ---------------------------------------------------------------------------
//  Test-fixture helpers
// ---------------------------------------------------------------------------

// Build a minimal valid DatasetSchema with one column "val".
DatasetSchema make_schema(Role role = Role::Feature) {
  DatasetSchema s;
  s.columns = {"val"};
  s.dtypes = {ColumnDType::F64};
  s.role = role;
  s.pit_delay = 0;
  s.region = "US";
  s.universe_tag = "test";
  return s;
}

// Build a Dataset with the given dates, one instrument (id=1), one column.
// Column values: value[date_row] = static_cast<f64>(date_row + 1) (1-based).
Dataset make_dataset(std::vector<DateKey> dates, Role role = Role::Feature) {
  const atx::usize n = dates.size();
  std::vector<atx::f64> col_data(n);
  for (atx::usize i = 0; i < n; ++i) {
    col_data[i] = static_cast<atx::f64>(i + 1); // 1, 2, 3, ...
  }
  auto res = Dataset::create(make_schema(role), std::move(dates), {InstKey{1}},
                             {std::move(col_data)}, {}, {"test", ""});
  return std::move(res).value();
}

// Build a Dataset with explicit per-row values (one instrument).
Dataset make_dataset_vals(std::vector<DateKey> dates, std::vector<atx::f64> vals,
                          Role role = Role::Feature) {
  auto res = Dataset::create(make_schema(role), std::move(dates), {InstKey{1}}, {std::move(vals)},
                             {}, {"test", ""});
  return std::move(res).value();
}

// ---------------------------------------------------------------------------
//  Tests
// ---------------------------------------------------------------------------

TEST(DataCatalog, RegisterRejectsDuplicate) {
  DatasetCatalog cat;

  auto s1 = cat.register_dataset("ds", make_dataset({10, 20, 30}));
  ASSERT_TRUE(s1.has_value()) << "first registration must succeed";

  auto s2 = cat.register_dataset("ds", make_dataset({10, 20, 30}));
  EXPECT_TRUE(!s2.has_value()) << "second registration with same name must fail";
  EXPECT_EQ(s2.error().code(), ErrorCode::InvalidArgument);
}

TEST(DataCatalog, RegisterRejectsNonAscendingDates) {
  DatasetCatalog cat;

  // Build a dataset with non-ascending dates {3, 1, 2}.
  // Dataset::create does NOT validate ordering — that's catalog's job.
  std::vector<atx::f64> vals = {1.0, 2.0, 3.0};
  auto ds_res = Dataset::create(make_schema(), {DateKey{3}, DateKey{1}, DateKey{2}}, {InstKey{1}},
                                {std::move(vals)}, {}, {"test", ""});
  ASSERT_TRUE(ds_res.has_value()) << "Dataset::create should accept any date order";

  auto s = cat.register_dataset("bad", std::move(ds_res).value());
  EXPECT_TRUE(!s.has_value()) << "register_dataset must reject non-ascending dates";
  EXPECT_EQ(s.error().code(), ErrorCode::InvalidArgument);
}

TEST(DataCatalog, ResolveReturnsRegistered) {
  DatasetCatalog cat;
  ASSERT_TRUE(
      cat.register_dataset("prices", make_dataset_vals({10, 20, 30}, {1.5, 2.5, 3.5})).has_value());

  // Successful resolve: check a column value via the reference wrapper.
  auto res = cat.resolve("prices");
  ASSERT_TRUE(res.has_value());
  const Dataset &ds = res->get();
  EXPECT_EQ(ds.num_dates(), 3u);
  // column(0)[0] == 1.5  (first row, first instrument)
  EXPECT_DOUBLE_EQ(ds.column(0)[0], 1.5);

  // Unknown name must return NotFound.
  auto nope = cat.resolve("nope");
  ASSERT_FALSE(nope.has_value());
  EXPECT_EQ(nope.error().code(), ErrorCode::NotFound);
}

TEST(DataCatalog, NamesAreAscendingDeterministic) {
  DatasetCatalog cat;
  // Register out of order: "c", "a", "b".
  ASSERT_TRUE(cat.register_dataset("c", make_dataset({1, 2})).has_value());
  ASSERT_TRUE(cat.register_dataset("a", make_dataset({1, 2})).has_value());
  ASSERT_TRUE(cat.register_dataset("b", make_dataset({1, 2})).has_value());

  const std::vector<std::string> expected{"a", "b", "c"};

  auto first_call = cat.names();
  EXPECT_EQ(first_call, expected);

  auto second_call = cat.names();
  EXPECT_EQ(second_call, expected); // identical on repeat call
}

TEST(DataCatalog, RoleOfReturnsSchemaRole) {
  DatasetCatalog cat;
  ASSERT_TRUE(cat.register_dataset("price_ds", make_dataset({1, 2}, Role::Price)).has_value());
  ASSERT_TRUE(cat.register_dataset("sig_ds", make_dataset({1, 2}, Role::Signal)).has_value());

  auto r1 = cat.role_of("price_ds");
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(*r1, Role::Price);

  auto r2 = cat.role_of("sig_ds");
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(*r2, Role::Signal);

  auto r_bad = cat.role_of("unknown");
  ASSERT_FALSE(r_bad.has_value());
  EXPECT_EQ(r_bad.error().code(), ErrorCode::NotFound);
}

TEST(DataCatalog, LineageTracksDerivation) {
  DatasetCatalog cat;
  ASSERT_TRUE(cat.register_dataset("a", make_dataset({1, 2})).has_value());
  ASSERT_TRUE(cat.register_dataset("b", make_dataset({1, 2})).has_value());
  ASSERT_TRUE(cat.register_dataset("child", make_dataset({1, 2})).has_value());

  // derive with parents in non-ascending order: {"b","a"} → stored as {"a","b"}.
  auto d = cat.derive("child", {"b", "a"});
  ASSERT_TRUE(d.has_value()) << d.error().message();

  auto lin = cat.lineage("child");
  ASSERT_TRUE(lin.has_value());
  EXPECT_EQ(*lin, (std::vector<std::string>{"a", "b"}));

  // A registered dataset with no derivation recorded → empty Ok.
  auto lin_a = cat.lineage("a");
  ASSERT_TRUE(lin_a.has_value());
  EXPECT_TRUE(lin_a->empty());

  // Unregistered parent → Err.
  auto bad = cat.derive("child", {"a", "not_registered"});
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().code(), ErrorCode::NotFound);
}

TEST(DataCatalog, AsOfResolvesLatestNotAfterDate) {
  // One dataset, dates {10, 20, 30}, values {100.0, 200.0, 300.0}.
  DatasetCatalog cat;
  ASSERT_TRUE(cat.register_dataset("ds", make_dataset_vals({10, 20, 30}, {100.0, 200.0, 300.0}))
                  .has_value());

  // canonical=25 → greatest date ≤ 25 is 20 → value 200.0
  auto v25 = cat.value_at("ds", "val", DateKey{25}, InstKey{1});
  ASSERT_TRUE(v25.has_value());
  EXPECT_DOUBLE_EQ(*v25, 200.0);

  // canonical=30 → date 30 qualifies exactly → value 300.0
  auto v30 = cat.value_at("ds", "val", DateKey{30}, InstKey{1});
  ASSERT_TRUE(v30.has_value());
  EXPECT_DOUBLE_EQ(*v30, 300.0);
}

TEST(DataCatalog, AsOfNeverLeaksRestatedFuture) {
  // Same dataset as above: dates {10, 20, 30}.
  DatasetCatalog cat;
  ASSERT_TRUE(cat.register_dataset("ds", make_dataset_vals({10, 20, 30}, {100.0, 200.0, 300.0}))
                  .has_value());

  // canonical=15 → greatest date ≤ 15 is 10 → value 100.0 (NOT 200.0).
  auto v15 = cat.value_at("ds", "val", DateKey{15}, InstKey{1});
  ASSERT_TRUE(v15.has_value());
  EXPECT_DOUBLE_EQ(*v15, 100.0);

  // canonical=5 → before first date → NaN, not Err.
  auto v5 = cat.value_at("ds", "val", DateKey{5}, InstKey{1});
  ASSERT_TRUE(v5.has_value());
  EXPECT_TRUE(std::isnan(*v5));
}

TEST(DataCatalog, ValueAtUnknownColumnOrInstErrs) {
  DatasetCatalog cat;
  ASSERT_TRUE(cat.register_dataset("ds", make_dataset_vals({10, 20}, {1.0, 2.0})).has_value());

  // Bad column name.
  auto bad_col = cat.value_at("ds", "no_such_col", DateKey{10}, InstKey{1});
  ASSERT_FALSE(bad_col.has_value());
  EXPECT_EQ(bad_col.error().code(), ErrorCode::NotFound);

  // Bad instrument key (instruments axis only contains InstKey{1}).
  auto bad_inst = cat.value_at("ds", "val", DateKey{10}, InstKey{999});
  ASSERT_FALSE(bad_inst.has_value());
  EXPECT_EQ(bad_inst.error().code(), ErrorCode::NotFound);
}

} // namespace
