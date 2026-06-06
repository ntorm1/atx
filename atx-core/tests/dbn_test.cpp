#include "atx/external/dbn.hpp"

#include <cstdint>

#include <gtest/gtest.h>

#include "dbn_fixture.hpp"

namespace dbn = atx::external::dbn;

TEST(DbnLayout, ConstantsMatchSpec) {
  // The wire-relevant constants must match the DBN spec. (Vocabulary types live
  // in namespace atx, not atx::external::dbn, so cast through std::uint8_t.)
  EXPECT_EQ(dbn::kFixedPriceScale, 1'000'000'000);
  EXPECT_EQ(static_cast<std::uint8_t>(dbn::RType::Ohlcv1D), 0x23);
  EXPECT_EQ(static_cast<std::uint8_t>(dbn::RType::OhlcvEod), 0x24);
}

TEST(DbnMetadata, ParsesHeaderAndDataset) {
  const auto bytes = atx::test::build_dbn({{101, "AAPL", 20240101, 20250101}}, /*rows=*/{});
  auto dec = dbn::DbnDecoder::open(bytes);
  ASSERT_TRUE(dec.has_value()) << dec.error().to_string();
  EXPECT_EQ(dec->metadata().version, 2);
  EXPECT_EQ(dec->metadata().dataset, "EQUS.SUMMARY");
  EXPECT_EQ(dec->metadata().symbol_cstr_len, 32);
}

TEST(DbnMetadata, RejectsBadMagic) {
  std::vector<std::byte> bad(64, std::byte(0));
  bad[0] = std::byte('X');
  auto dec = dbn::DbnDecoder::open(bad);
  EXPECT_FALSE(dec.has_value());
}

TEST(DbnMetadata, ResolvesSymbol) {
  const auto bytes = atx::test::build_dbn(
      {{101, "AAPL", 20240101, 20250101}, {102, "MSFT", 20240101, 20250101}}, {});
  auto dec = dbn::DbnDecoder::open(bytes);
  ASSERT_TRUE(dec.has_value());
  EXPECT_EQ(dec->symbol_for(101), "AAPL");
  EXPECT_EQ(dec->symbol_for(102), "MSFT");
  EXPECT_TRUE(dec->symbol_for(999).empty());
}

TEST(DbnRecords, DecodesOhlcvFieldsAndSkipsOthers) {
  using atx::test::OhlcvRow;
  const std::vector<OhlcvRow> rows = {
      {101, 1'700'000'000'000'000'000ULL, 1000, 1100, 990, 1050, 12345, 0x23},
      {201, 1'700'000'000'000'000'000ULL, 0, 0, 0, 0, 0, 0x12 /*status: unsupported*/},
      {102, 1'700'086'400'000'000'000ULL, 2000, 2200, 1980, 2100, 6789, 0x24},
  };
  const auto bytes = atx::test::build_dbn({{101, "AAPL", 20240101, 20250101}}, rows);
  auto dec = dbn::DbnDecoder::open(bytes);
  ASSERT_TRUE(dec.has_value());

  auto a = dec->next();
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(a->has_value());
  EXPECT_EQ((*a)->hd.instrument_id, 101u);
  EXPECT_EQ((*a)->open, 1000);
  EXPECT_EQ((*a)->close, 1050);
  EXPECT_EQ((*a)->volume, 12345u);

  auto b = dec->next(); // 0x12 skipped; returns the 0x24 row
  ASSERT_TRUE(b.has_value());
  ASSERT_TRUE(b->has_value());
  EXPECT_EQ((*b)->hd.instrument_id, 102u);
  EXPECT_EQ((*b)->hd.rtype, 0x24);

  auto end = dec->next();
  ASSERT_TRUE(end.has_value());
  EXPECT_FALSE(end->has_value());       // exhausted
  EXPECT_EQ(dec->skipped_records(), 1); // the 0x12 record
}
