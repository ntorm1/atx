// frame_test.cpp — TDD tests for atx::core::series::Frame
//
// Order: seed cases (from spec) + full coverage: duplicate-name rejection,
// wrong-type lookup rejection, missing-column lookup, num_columns / has_column,
// heterogeneous columns coexisting, the const column() overload, the shared
// Timestamp index, and the Slice row-range descriptor.

#include <atx/core/series/frame.hpp>

#include <gtest/gtest.h>

#include <atx/core/datetime.hpp> // atx::core::time::Timestamp

using atx::core::series::Column;
using atx::core::series::Frame;
using atx::core::time::Timestamp;
using atx::usize;
using namespace atx::core; // ErrorCode

// ============================================================
// Seed tests (from spec)
// ============================================================

TEST(Frame, AddColumnsAndRows) {
    Frame f;
    // reference_wrapper<Column<double>> implicitly converts to Column<double>&.
    Column<double>& px  = f.add_column<double>("price").value();
    Column<double>& vol = f.add_column<double>("volume").value();
    px.append(10.0);
    vol.append(100.0);
    EXPECT_EQ(f.rows(), 1U);
    EXPECT_DOUBLE_EQ(f.column<double>("price").value().get().view()[0], 10.0);
    EXPECT_DOUBLE_EQ(f.column<double>("volume").value().get().view()[0], 100.0);
}

TEST(Frame, MissingColumnErrs) {
    Frame f;
    EXPECT_FALSE(f.column<double>("nope").has_value());
    EXPECT_EQ(f.column<double>("nope").error().code(), ErrorCode::NotFound);
}

TEST(Frame, RowCountInvariantAcrossColumns) {
    Frame f;
    Column<double>& a = f.add_column<double>("a").value();
    a.append(1.0);
    a.append(2.0);
    EXPECT_EQ(f.rows(), 2U);
}

// ============================================================
// add_column — duplicate name rejection
// ============================================================

TEST(Frame, AddDuplicateColumnNameErrs) {
    Frame f;
    ASSERT_TRUE(f.add_column<double>("price").has_value());
    const auto dup = f.add_column<double>("price");
    EXPECT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error().code(), ErrorCode::AlreadyExists);
}

TEST(Frame, AddDuplicateNameDifferentTypeStillErrs) {
    Frame f;
    ASSERT_TRUE(f.add_column<double>("x").has_value());
    const auto dup = f.add_column<int>("x"); // same name, different type
    EXPECT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error().code(), ErrorCode::AlreadyExists);
}

// ============================================================
// column — wrong-type lookup rejection
// ============================================================

TEST(Frame, ColumnWrongTypeErrs) {
    Frame f;
    ASSERT_TRUE(f.add_column<double>("price").has_value());
    const auto wrong = f.column<int>("price"); // stored as double
    EXPECT_FALSE(wrong.has_value());
    EXPECT_EQ(wrong.error().code(), ErrorCode::InvalidArgument);
}

TEST(Frame, ColumnCorrectTypeAfterWrongTypeStillWorks) {
    Frame f;
    f.add_column<double>("price").value().get().append(7.5);
    EXPECT_FALSE(f.column<int>("price").has_value());     // wrong type
    ASSERT_TRUE(f.column<double>("price").has_value());   // correct type
    EXPECT_DOUBLE_EQ(f.column<double>("price").value().get().view()[0], 7.5);
}

// ============================================================
// Observers — num_columns / has_column
// ============================================================

TEST(Frame, NumColumnsCountsAddedColumns) {
    Frame f;
    EXPECT_EQ(f.num_columns(), 0U);
    f.add_column<double>("a").value();
    f.add_column<int>("b").value();
    EXPECT_EQ(f.num_columns(), 2U);
}

TEST(Frame, HasColumnReflectsMembership) {
    Frame f;
    EXPECT_FALSE(f.has_column("a"));
    f.add_column<double>("a").value();
    EXPECT_TRUE(f.has_column("a"));
    EXPECT_FALSE(f.has_column("b"));
}

// ============================================================
// Heterogeneous columns coexist
// ============================================================

TEST(Frame, HeterogeneousColumnsCoexist) {
    Frame f;
    Column<double>& d = f.add_column<double>("d").value();
    Column<int>&    i = f.add_column<int>("i").value();
    d.append(3.14);
    i.append(42);
    EXPECT_DOUBLE_EQ(f.column<double>("d").value().get().view()[0], 3.14);
    EXPECT_EQ(f.column<int>("i").value().get().view()[0], 42);
    EXPECT_EQ(f.num_columns(), 2U);
}

// ============================================================
// rows() — max column length across uneven columns
// ============================================================

TEST(Frame, RowsIsMaxColumnLength) {
    Frame f;
    Column<double>& a = f.add_column<double>("a").value();
    Column<int>&    b = f.add_column<int>("b").value();
    a.append(1.0);
    a.append(2.0);
    a.append(3.0);
    b.append(9); // shorter column
    EXPECT_EQ(f.rows(), 3U);
}

TEST(Frame, RowsOfEmptyFrameIsZero) {
    Frame f;
    EXPECT_EQ(f.rows(), 0U);
}

// ============================================================
// const column() overload
// ============================================================

TEST(Frame, ConstColumnOverloadReads) {
    Frame f;
    f.add_column<double>("price").value().get().append(5.0);
    const Frame& cf = f;
    const auto r = cf.column<double>("price");
    ASSERT_TRUE(r.has_value());
    EXPECT_DOUBLE_EQ(r.value().get().view()[0], 5.0);
}

TEST(Frame, ConstColumnMissingErrs) {
    const Frame f;
    const auto r = f.column<double>("nope");
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::NotFound);
}

TEST(Frame, ConstColumnWrongTypeErrs) {
    Frame f;
    f.add_column<double>("price").value();
    const Frame& cf = f;
    const auto r = cf.column<int>("price");
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

// ============================================================
// Shared Timestamp index
// ============================================================

TEST(Frame, IndexStartsEmpty) {
    Frame f;
    EXPECT_EQ(f.index().size(), 0U);
}

TEST(Frame, AppendIndexGrowsIndexAndCountsTowardRows) {
    Frame f;
    f.append_index(Timestamp::from_unix_nanos(1000));
    f.append_index(Timestamp::from_unix_nanos(2000));
    EXPECT_EQ(f.index().size(), 2U);
    EXPECT_EQ(f.index().view()[0].unix_nanos(), 1000);
    EXPECT_EQ(f.index().view()[1].unix_nanos(), 2000);
    // rows() counts the index length as well.
    EXPECT_EQ(f.rows(), 2U);
}

TEST(Frame, ConstIndexAccessor) {
    Frame f;
    f.append_index(Timestamp::from_unix_nanos(42));
    const Frame& cf = f;
    ASSERT_EQ(cf.index().size(), 1U);
    EXPECT_EQ(cf.index().view()[0].unix_nanos(), 42);
}

// ============================================================
// Slice — lightweight row-range descriptor
// ============================================================

TEST(Frame, SliceDescribesRowRange) {
    Frame f;
    Column<double>& a = f.add_column<double>("a").value();
    for (int i = 0; i < 5; ++i) {
        a.append(static_cast<double>(i));
    }
    const Frame::Slice s = f.slice(1U, 4U);
    EXPECT_EQ(s.begin, 1U);
    EXPECT_EQ(s.end, 4U);
}

// ============================================================
// Move-only semantics
// ============================================================

TEST(Frame, MoveTransfersColumns) {
    Frame src;
    src.add_column<double>("price").value().get().append(11.0);
    Frame dst{std::move(src)};
    ASSERT_TRUE(dst.column<double>("price").has_value());
    EXPECT_DOUBLE_EQ(dst.column<double>("price").value().get().view()[0], 11.0);
    EXPECT_EQ(dst.num_columns(), 1U);
}
