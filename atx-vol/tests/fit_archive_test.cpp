// ATXVFA framing: checksums, layout, atomic publish, open modes, schema
// evolution, and one crafted file per refusal (ARCHIVE_FORMAT.md section 8).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/storage/fit_archive.hpp"
#include "atx/vol/api/storage/fit_archive_records.hpp"
#include "support/fit_archive_fixture.hpp"

namespace atx::vol {
namespace {

using testkit::find_section;
using testkit::join_image;
using testkit::open_status;
using testkit::read_at;
using testkit::reseal;
using testkit::split_image;
using testkit::write_at;
using Type = FitArchiveSectionType;

constexpr std::size_t kHeader = sizeof(FitArchiveHeader);
constexpr std::size_t kEntry = sizeof(FitArchiveSectionEntry);
constexpr std::size_t kFooter = sizeof(FitArchiveFooter);

// One graded surface (three expiries), cached for the whole test binary.
const std::vector<std::byte> &sample_image() {
  static const std::vector<std::byte> image = [] {
    const auto surface = testkit::synthetic_surface();
    FitArchiveWriter w({1792000000000000000LL, 7, 0});
    const FitArchiveSurfaceInput in{"AAPL", 1791988200000000000LL, surface.slices, surface.expiry_days,
                                    surface.config, {}, "ordinary", "raw-liquidity"};
    EXPECT_TRUE(w.add_surface(in).has_value());
    auto bytes = w.finish();
    EXPECT_TRUE(bytes.has_value());
    return bytes ? std::move(*bytes) : std::vector<std::byte>{};
  }();
  return image;
}

std::size_t entry_index(const std::vector<std::byte> &image, Type type) {
  const auto h = read_at<FitArchiveHeader>(image, 0);
  for (std::size_t i = 0; i < h.section_count; ++i) {
    if (read_at<FitArchiveSectionEntry>(image, kHeader + i * kEntry).type == static_cast<std::uint32_t>(type)) {
      return i;
    }
  }
  ADD_FAILURE() << "section absent";
  return 0;
}

// Edit one section-table entry, then fix the table CRC and reseal, so only the
// edited field is wrong.
template <class F> void edit_entry(std::vector<std::byte> &image, std::size_t i, F fn) {
  auto e = read_at<FitArchiveSectionEntry>(image, kHeader + i * kEntry);
  fn(e);
  write_at(image, kHeader + i * kEntry, e);
  auto h = read_at<FitArchiveHeader>(image, 0);
  h.section_table_crc32c =
      fit_archive_crc32c(std::span<const std::byte>{image.data() + kHeader, h.section_count * kEntry});
  write_at(image, 0, h);
  reseal(image);
}

template <class F> void edit_header(std::vector<std::byte> &image, F fn) {
  auto h = read_at<FitArchiveHeader>(image, 0);
  fn(h);
  write_at(image, 0, h);
  reseal(image);
}

TEST(FitArchive, Crc32cKnownAnswer) {
  constexpr std::string_view text{"123456789"};
  EXPECT_EQ(fit_archive_crc32c(std::as_bytes(std::span{text.data(), text.size()})), 0xE3069283u);
}

TEST(FitArchive, EmptyArchiveFramesAndOpens) {
  FitArchiveWriter w({1234, 7, 0});
  const auto image = w.finish();
  ASSERT_TRUE(image.has_value());
  EXPECT_EQ(image->size() % kFitArchiveSectionAlign, 0u);
  auto a = FitArchive::open_copy(*image, FitArchiveVerify::Paranoid);
  ASSERT_TRUE(a.has_value()) << to_string(a.error().status);
  EXPECT_EQ(a->header().format_major, kFitArchiveFormatMajor);
  EXPECT_EQ(a->header().created_unix_ns, 1234);
  EXPECT_EQ(a->header().archive_id, 7u);
  EXPECT_EQ(a->header().file_size, image->size());
  EXPECT_EQ(a->footer().file_size, image->size());
  EXPECT_EQ(a->footer().header_crc32c, a->header().header_crc32c);
  EXPECT_EQ(std::string_view(a->header().writer_name.data()).substr(0, 8), "atx-vol ");
  ASSERT_EQ(a->sections().size(), 2u);
  EXPECT_TRUE(a->symbols().empty());
  EXPECT_TRUE(a->fits().empty());
  EXPECT_EQ(a->find_fit(FitArchiveKey{}), nullptr);
  EXPECT_FALSE(a->find_underlier("AAPL").has_value());
  EXPECT_TRUE(a->verify_file_checksum().has_value());
}

TEST(FitArchive, SectionsAre64ByteAlignedChecksummedAndZeroPadded) {
  const auto &image = sample_image();
  auto a = FitArchive::open_copy(image, FitArchiveVerify::Paranoid);
  ASSERT_TRUE(a.has_value()) << to_string(a.error().status);
  std::uint64_t cursor = kHeader + a->sections().size() * kEntry;
  for (std::size_t i = 0; i < a->sections().size(); ++i) {
    const auto &e = a->sections()[i];
    EXPECT_EQ(e.offset % 64u, 0u) << to_string(static_cast<Type>(e.type));
    EXPECT_EQ(e.length, e.element_count * e.element_stride);
    EXPECT_TRUE(a->verify_section(i).has_value());
    for (std::uint64_t at = cursor; at < e.offset; ++at) EXPECT_EQ(image[at], std::byte{0});
    cursor = e.offset + e.length;
  }
  EXPECT_EQ(image.size() - kFooter, (cursor + 63u) / 64u * 64u);
  EXPECT_FALSE(a->verify_section(a->sections().size()).has_value());
}

TEST(FitArchive, PublishIsAtomicAndBothOpenModesAgree) {
  const auto dir = testkit::scratch_dir("publish");
  const auto path = (dir / "one.atxvfa").string();
  ASSERT_TRUE(publish_fit_archive(path, sample_image()).has_value());
  for (const auto &entry : std::filesystem::directory_iterator(dir)) {
    EXPECT_EQ(entry.path().filename().string(), "one.atxvfa") << "temp left behind";
  }
  {
    auto mapped = FitArchive::open(path, {FitArchiveOpenMode::Mapped, FitArchiveVerify::Paranoid});
    auto read = FitArchive::open(path, {FitArchiveOpenMode::Read, FitArchiveVerify::Paranoid});
    ASSERT_TRUE(mapped.has_value()) << to_string(mapped.error().status);
    ASSERT_TRUE(read.has_value()) << to_string(read.error().status);
    ASSERT_EQ(mapped->bytes().size(), sample_image().size());
    EXPECT_TRUE(std::equal(mapped->bytes().begin(), mapped->bytes().end(), sample_image().begin()));
    EXPECT_TRUE(std::equal(read->bytes().begin(), read->bytes().end(), sample_image().begin()));
    EXPECT_EQ(mapped->fits().size(), 3u);
  }
  // Republish over the same path (no reader maps it any more) replaces it atomically.
  FitArchiveWriter empty;
  ASSERT_TRUE(empty.write_file(path).has_value());
  auto again = FitArchive::open(path);
  ASSERT_TRUE(again.has_value());
  EXPECT_TRUE(again->fits().empty());
}

TEST(FitArchive, MissingFileIsNotFound) {
  const auto dir = testkit::scratch_dir("missing");
  auto a = FitArchive::open((dir / "absent.atxvfa").string());
  ASSERT_FALSE(a.has_value());
  EXPECT_EQ(a.error().status, ArchiveStatus::NotFound);
}

TEST(FitArchive, EmptyFileIsTruncatedInBothModes) {
  const auto dir = testkit::scratch_dir("empty");
  const auto path = (dir / "empty.atxvfa").string();
  { std::ofstream touch(path, std::ios::binary); }
  for (const auto mode : {FitArchiveOpenMode::Mapped, FitArchiveOpenMode::Read}) {
    auto a = FitArchive::open(path, {mode, FitArchiveVerify::Full});
    ASSERT_FALSE(a.has_value());
    EXPECT_EQ(a.error().status, ArchiveStatus::Truncated);
  }
}

TEST(FitArchive, MisalignedBytesRefused) {
  std::vector<std::uint64_t> storage(sample_image().size() / 8 + 1);
  auto *base = reinterpret_cast<std::byte *>(storage.data()) + 1; // NOLINT: deliberate misalignment
  std::memcpy(base, sample_image().data(), sample_image().size());
  auto a = FitArchive::open_bytes({base, sample_image().size()}, nullptr);
  ASSERT_FALSE(a.has_value());
  EXPECT_EQ(a.error().status, ArchiveStatus::Misaligned);
}

// ── Framing refusals ─────────────────────────────────────────────────────────

TEST(FitArchiveRefusal, TruncatedFromEitherEnd) {
  auto cut = sample_image();
  cut.resize(cut.size() - kFooter);
  EXPECT_EQ(open_status(cut), ArchiveStatus::Truncated);
  auto half = sample_image();
  half.resize(half.size() / 2);
  EXPECT_EQ(open_status(half), ArchiveStatus::Truncated);
  const std::vector<std::byte> stub(sample_image().begin(), sample_image().begin() + 10);
  EXPECT_EQ(open_status(stub), ArchiveStatus::Truncated);
  // Cut, then padded back to the declared size: the footer is gone.
  auto padded = sample_image();
  std::fill(padded.end() - static_cast<std::ptrdiff_t>(kFooter), padded.end(), std::byte{0});
  EXPECT_EQ(open_status(padded), ArchiveStatus::FooterInvalid);
}

TEST(FitArchiveRefusal, TrailingBytesAreSizeMismatch) {
  auto longer = sample_image();
  longer.resize(longer.size() + 64);
  EXPECT_EQ(open_status(longer), ArchiveStatus::SizeMismatch);
}

TEST(FitArchiveRefusal, ForeignFileIsBadMagic) {
  auto image = sample_image();
  image[1] = std::byte{'X'};
  EXPECT_EQ(open_status(image), ArchiveStatus::BadMagic);
  const std::vector<std::byte> text(400, std::byte{'a'});
  EXPECT_EQ(open_status(text), ArchiveStatus::BadMagic);
}

TEST(FitArchiveRefusal, ForeignEndianAndMajor) {
  auto endian = sample_image();
  edit_header(endian, [](FitArchiveHeader &h) { h.endian_tag = 0x04030201u; });
  EXPECT_EQ(open_status(endian), ArchiveStatus::UnsupportedEndian);
  auto major = sample_image();
  edit_header(major, [](FitArchiveHeader &h) { h.format_major = kFitArchiveFormatMajor + 1; });
  auto a = FitArchive::open_copy(major);
  ASSERT_FALSE(a.has_value());
  EXPECT_EQ(a.error().status, ArchiveStatus::UnsupportedMajor);
  EXPECT_EQ(a.error().detail, kFitArchiveFormatMajor + 1u);
}

TEST(FitArchiveRefusal, HeaderFieldsAndChecksum) {
  auto size = sample_image();
  edit_header(size, [](FitArchiveHeader &h) { h.header_size = 255; });
  EXPECT_EQ(open_status(size), ArchiveStatus::HeaderInvalid);
  auto reserved = sample_image();
  edit_header(reserved, [](FitArchiveHeader &h) { h.reserved[40] = 1; });
  EXPECT_EQ(open_status(reserved), ArchiveStatus::HeaderInvalid);
  auto count = sample_image();
  edit_header(count, [](FitArchiveHeader &h) { h.section_count = kFitArchiveMaxSections + 1; });
  EXPECT_EQ(open_status(count), ArchiveStatus::HeaderInvalid);
  auto flipped = sample_image();
  flipped[offsetof(FitArchiveHeader, created_unix_ns)] ^= std::byte{1};
  EXPECT_EQ(open_status(flipped), ArchiveStatus::HeaderChecksum);
}

TEST(FitArchiveRefusal, FooterFieldsAndChecksum) {
  const std::size_t at = sample_image().size() - kFooter;
  auto magic = sample_image();
  magic[at] = std::byte{'Q'};
  EXPECT_EQ(open_status(magic), ArchiveStatus::FooterInvalid);
  auto crc = sample_image();
  crc[at + offsetof(FitArchiveFooter, reserved)] = std::byte{9};
  EXPECT_EQ(open_status(crc), ArchiveStatus::FooterChecksum);
  auto copy = sample_image();
  auto f = read_at<FitArchiveFooter>(copy, at);
  f.segment_sequence = 99;
  f.footer_crc32c = 0;
  f.footer_crc32c = fit_archive_crc32c(std::as_bytes(std::span{&f, 1}));
  write_at(copy, at, f);
  EXPECT_EQ(open_status(copy), ArchiveStatus::FooterMismatch);
}

TEST(FitArchiveRefusal, SectionTable) {
  auto crc = sample_image();
  crc[kHeader + offsetof(FitArchiveSectionEntry, reserved)] = std::byte{1};
  EXPECT_EQ(open_status(crc), ArchiveStatus::SectionTableChecksum);
  auto align = sample_image();
  edit_entry(align, 1, [](FitArchiveSectionEntry &e) { e.offset += 8; });
  EXPECT_EQ(open_status(align), ArchiveStatus::SectionAlignment);
  auto bounds = sample_image();
  edit_entry(bounds, 1, [](FitArchiveSectionEntry &e) { e.length = 1u << 30; e.element_count = e.length; });
  EXPECT_EQ(open_status(bounds), ArchiveStatus::SectionBounds);
  auto overlap = sample_image();
  const auto first = read_at<FitArchiveSectionEntry>(overlap, kHeader);
  ASSERT_GT(first.length, 0u);
  edit_entry(overlap, 1, [&](FitArchiveSectionEntry &e) { e.offset = first.offset; });
  EXPECT_EQ(open_status(overlap), ArchiveStatus::SectionOverlap);
  auto stride = sample_image();
  edit_entry(stride, 0, [](FitArchiveSectionEntry &e) { e.element_count += 1; });
  EXPECT_EQ(open_status(stride), ArchiveStatus::SectionStride);
  auto reserved = sample_image();
  edit_entry(reserved, 0, [](FitArchiveSectionEntry &e) { e.reserved[3] = 1; });
  EXPECT_EQ(open_status(reserved), ArchiveStatus::HeaderInvalid);
}

TEST(FitArchiveRefusal, UnknownTypesVersionsAndDuplicates) {
  const std::array<std::byte, 8> payload{};
  const auto frame = [&](std::vector<FitArchiveSectionInput> extra) {
    std::vector<FitArchiveSectionInput> s{{Type::Symbols, 1, 0, 48, 8, kFitArchiveSectionRequired, {}},
                                          {Type::Strings, 1, 0, 1, 1, kFitArchiveSectionRequired, {}}};
    s.insert(s.end(), extra.begin(), extra.end());
    auto image = build_fit_archive(s, {});
    EXPECT_TRUE(image.has_value());
    return image ? *image : std::vector<std::byte>{};
  };
  const auto unknown = static_cast<Type>(90);
  EXPECT_EQ(open_status(frame({{unknown, 1, 0, 1, 1, 0u, payload}})), ArchiveStatus::Ok);
  EXPECT_EQ(open_status(frame({{unknown, 1, 0, 1, 1, kFitArchiveSectionRequired, payload}})),
            ArchiveStatus::UnsupportedSection);
  EXPECT_EQ(open_status(frame({{Type::Provenance, 1, 0, 1, 1, 2u, payload}})), ArchiveStatus::UnsupportedSection);
  EXPECT_EQ(open_status(frame({{Type::Provenance, 2, 0, 1, 1, 0u, payload}})),
            ArchiveStatus::UnsupportedRecordVersion);
  // The builder refuses a duplicate type; a reader refuses one crafted in place.
  const std::vector<FitArchiveSectionInput> dup{{Type::Strings, 1, 0, 1, 1, 0u, payload},
                                                {Type::Strings, 1, 0, 1, 1, 0u, payload}};
  auto refused = build_fit_archive(dup, {});
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().status, ArchiveStatus::DuplicateSection);
  auto image = frame({{Type::Provenance, 1, 0, 1, 1, 0u, payload}, {static_cast<Type>(4), 1, 0, 1, 1, 0u, payload}});
  edit_entry(image, 3, [](FitArchiveSectionEntry &e) { e.type = static_cast<std::uint32_t>(Type::Provenance); });
  EXPECT_EQ(open_status(image), ArchiveStatus::DuplicateSection);
}

TEST(FitArchiveRefusal, PayloadChecksumPaddingAndFileChecksumByVerifyLevel) {
  // A flipped string byte is invisible structurally: only the CRC catches it.
  auto payload = sample_image();
  const auto strings = read_at<FitArchiveSectionEntry>(payload, kHeader + entry_index(payload, Type::Strings) * kEntry);
  ASSERT_GT(strings.length, 0u);
  payload[strings.offset] ^= std::byte{0x20};
  reseal(payload);
  EXPECT_EQ(open_status(payload, FitArchiveVerify::Full), ArchiveStatus::SectionChecksum);
  EXPECT_EQ(open_status(payload, FitArchiveVerify::Framing), ArchiveStatus::Ok);

  auto padding = sample_image();
  ASSERT_NE(strings.length % 64u, 0u) << "the sample needs a padded section";
  padding[strings.offset + strings.length] = std::byte{1};
  reseal(padding);
  EXPECT_EQ(open_status(padding, FitArchiveVerify::Full), ArchiveStatus::PaddingNotZero);
  EXPECT_EQ(open_status(padding, FitArchiveVerify::Framing), ArchiveStatus::Ok);

  auto file = sample_image();
  const std::size_t at = file.size() - kFooter;
  auto f = read_at<FitArchiveFooter>(file, at);
  f.file_crc32c ^= 1u;
  f.footer_crc32c = 0;
  f.footer_crc32c = fit_archive_crc32c(std::as_bytes(std::span{&f, 1}));
  write_at(file, at, f);
  EXPECT_EQ(open_status(file, FitArchiveVerify::Full), ArchiveStatus::Ok);
  EXPECT_EQ(open_status(file, FitArchiveVerify::Paranoid), ArchiveStatus::FileChecksum);
}

// ── Record-pass refusals (re-framed with fresh checksums) ───────────────────

TEST(FitArchiveRefusal, MissingIndex) {
  auto sections = split_image(sample_image());
  sections.erase(std::remove_if(sections.begin(), sections.end(),
                                [](const auto &s) { return s.meta.type == Type::FitIndex; }),
                 sections.end());
  EXPECT_EQ(open_status(join_image(sections)), ArchiveStatus::MissingSection);
}

TEST(FitArchiveRefusal, IndexOrderAndTargets) {
  auto unsorted = split_image(sample_image());
  auto *index = find_section(unsorted, Type::FitIndex);
  ASSERT_NE(index, nullptr);
  ASSERT_GE(index->bytes.size(), 2 * sizeof(FitArchiveIndexEntry));
  std::swap_ranges(index->bytes.begin(), index->bytes.begin() + 24, index->bytes.begin() + 24);
  EXPECT_EQ(open_status(join_image(unsorted)), ArchiveStatus::IndexUnsorted);

  auto target = split_image(sample_image());
  index = find_section(target, Type::FitIndex);
  write_at<std::uint32_t>(index->bytes, offsetof(FitArchiveIndexEntry, record), 99u);
  EXPECT_EQ(open_status(join_image(target)), ArchiveStatus::IndexBounds);

  auto symbols = split_image(sample_image());
  auto *sym = find_section(symbols, Type::Symbols);
  write_at<std::uint32_t>(sym->bytes, offsetof(FitArchiveSymbol, length), 41u);
  EXPECT_EQ(open_status(join_image(symbols)), ArchiveStatus::RecordInvalid);
}

TEST(FitArchiveRefusal, RecordReferencesEnumsAndReservedBytes) {
  const auto mutate_fit = [](auto fn) {
    auto sections = split_image(sample_image());
    auto *fits = find_section(sections, Type::FitRecords);
    EXPECT_NE(fits, nullptr);
    auto r = read_at<FitArchiveFitRecord>(fits->bytes, 0);
    fn(r);
    write_at(fits->bytes, 0, r);
    return open_status(join_image(sections));
  };
  EXPECT_EQ(mutate_fit([](FitArchiveFitRecord &r) { r.rows.first = 1u << 20; }), ArchiveStatus::RecordBounds);
  EXPECT_EQ(mutate_fit([](FitArchiveFitRecord &r) { r.fit_error.length = 1u << 20; }), ArchiveStatus::RecordBounds);
  EXPECT_EQ(mutate_fit([](FitArchiveFitRecord &r) { r.fit_status = 9; }), ArchiveStatus::RecordInvalid);
  {
    auto sections = split_image(sample_image());
    const auto r = read_at<FitArchiveFitRecord>(find_section(sections, Type::FitRecords)->bytes, 0);
    ASSERT_EQ(r.curve.present, 1u) << "the first sample expiry must carry a curve";
  }
  EXPECT_EQ(mutate_fit([](FitArchiveFitRecord &r) { r.curve.kind = 6; }), ArchiveStatus::RecordInvalid);
  EXPECT_EQ(mutate_fit([](FitArchiveFitRecord &r) { r.curve.payload.count -= 1; }), ArchiveStatus::RecordInvalid);
  EXPECT_EQ(mutate_fit([](FitArchiveFitRecord &r) { r.slice.grade = 4; }), ArchiveStatus::RecordInvalid);
  EXPECT_EQ(mutate_fit([](FitArchiveFitRecord &r) { r.reserved[0] = 1; }), ArchiveStatus::RecordInvalid);
  EXPECT_EQ(mutate_fit([](FitArchiveFitRecord &r) { r.surface = 5; }), ArchiveStatus::RecordInvalid);
  EXPECT_EQ(mutate_fit([](FitArchiveFitRecord &r) { r.key.reserved = 1; }), ArchiveStatus::RecordInvalid);
  EXPECT_EQ(mutate_fit([](FitArchiveFitRecord &r) { r.prices.nodes.count = 1; }), ArchiveStatus::RecordInvalid);

  auto rows = split_image(sample_image());
  auto *quote_rows = find_section(rows, Type::QuoteRows);
  ASSERT_NE(quote_rows, nullptr);
  quote_rows->bytes[offsetof(FitArchiveQuoteRow, quote_status)] = std::byte{200};
  EXPECT_EQ(open_status(join_image(rows)), ArchiveStatus::RecordInvalid);

  auto surface = split_image(sample_image());
  auto *surfaces = find_section(surface, Type::SurfaceRecords);
  auto s = read_at<FitArchiveSurfaceRecord>(surfaces->bytes, 0);
  s.grade_counts[0] += 1;
  write_at(surfaces->bytes, 0, s);
  EXPECT_EQ(open_status(join_image(surface)), ArchiveStatus::RecordInvalid);
}

TEST(FitArchiveRefusal, WriterRefusesDuplicateKeysAndBadInput) {
  const auto surface = testkit::synthetic_surface();
  FitArchiveWriter w;
  const FitArchiveFitInput one{"MSFT", 5, 20800, 0, &surface.slices[0].fit};
  ASSERT_TRUE(w.add_fit(one).has_value());
  ASSERT_TRUE(w.add_fit(one).has_value());
  auto image = w.finish();
  ASSERT_FALSE(image.has_value());
  EXPECT_EQ(image.error().status, ArchiveStatus::DuplicateKey);
  FitArchiveWriter bad;
  EXPECT_FALSE(bad.add_fit({"", 5, 20800, 0, &surface.slices[0].fit}).has_value());
  EXPECT_FALSE(bad.add_fit({"A-SYMBOL-LONGER-THAN-FORTY-BYTES-IS-REFUSED", 5, 20800, 0,
                            &surface.slices[0].fit}).has_value());
  EXPECT_FALSE(bad.add_fit({"MSFT", 5, 20800, 0, nullptr}).has_value());
  const std::array<std::int32_t, 2> days{1, 1};
  EXPECT_FALSE(bad.add_surface({"MSFT", 5, std::span(surface.slices).first(2), days}).has_value());
  EXPECT_EQ(bad.fit_count(), 0u) << "a failed add leaves the writer unchanged";
}

// ── Schema evolution ─────────────────────────────────────────────────────────

TEST(FitArchiveEvolution, NewerFormatMinorIsReadable) {
  auto image = sample_image();
  edit_header(image, [](FitArchiveHeader &h) { h.format_minor = kFitArchiveFormatMinor + 3; });
  auto a = FitArchive::open_copy(image);
  ASSERT_TRUE(a.has_value()) << to_string(a.error().status);
  EXPECT_EQ(a->fits().size(), 3u);
}

TEST(FitArchiveEvolution, NewerRecordMinorWithWiderStrideServesKnownPrefix) {
  auto sections = split_image(sample_image());
  auto *fits = find_section(sections, Type::FitRecords);
  ASSERT_NE(fits, nullptr);
  constexpr std::size_t kOld = sizeof(FitArchiveFitRecord);
  constexpr std::size_t kNew = kOld + 64;
  std::vector<std::byte> wider;
  for (std::size_t at = 0; at < fits->bytes.size(); at += kOld) {
    wider.insert(wider.end(), fits->bytes.begin() + static_cast<std::ptrdiff_t>(at),
                 fits->bytes.begin() + static_cast<std::ptrdiff_t>(at + kOld));
    wider.insert(wider.end(), 64, std::byte{0xAB}); // a future minor's fields
  }
  fits->bytes = std::move(wider);
  fits->meta.element_stride = kNew;
  fits->meta.type_minor = 1;
  auto a = FitArchive::open_copy(join_image(sections));
  ASSERT_TRUE(a.has_value()) << to_string(a.error().status);
  auto original = FitArchive::open_copy(sample_image());
  ASSERT_TRUE(original.has_value());
  ASSERT_EQ(a->fits().size(), original->fits().size());
  for (std::size_t i = 0; i < a->fits().size(); ++i) {
    EXPECT_EQ(std::memcmp(&a->fits()[i], &original->fits()[i], kOld), 0);
    auto x = decode_surface_slice(*a, a->fits()[i]);
    auto y = decode_surface_slice(*original, original->fits()[i]);
    ASSERT_TRUE(x.has_value() && y.has_value());
    EXPECT_EQ(testkit::diff_slice(*x, *y), "");
  }
}

// A file of minor 0 (before lane C0): 896-byte fit records, 48-byte quote rows, no
// PriceOnlyNodes section. It opens, and the minor-1 tails read as absent.
TEST(FitArchiveEvolution, MinorZeroFileReadsTheIdentifiabilityTailsAsAbsent) {
  auto sections = split_image(sample_image());
  const auto narrow = [](testkit::ImageSection &s, std::size_t old_stride) {
    const std::size_t stride = s.meta.element_stride;
    std::vector<std::byte> out;
    for (std::size_t at = 0; at < s.bytes.size(); at += stride) {
      out.insert(out.end(), s.bytes.begin() + static_cast<std::ptrdiff_t>(at),
                 s.bytes.begin() + static_cast<std::ptrdiff_t>(at + old_stride));
    }
    s.bytes = std::move(out);
    s.meta.element_stride = static_cast<std::uint32_t>(old_stride);
    s.meta.type_minor = 0;
  };
  auto *fits = find_section(sections, Type::FitRecords);
  auto *rows = find_section(sections, Type::QuoteRows);
  ASSERT_NE(fits, nullptr);
  ASSERT_NE(rows, nullptr);
  EXPECT_EQ(fits->meta.type_minor, 1u);
  EXPECT_EQ(rows->meta.type_minor, 1u);
  narrow(*fits, 896);
  narrow(*rows, 48);
  std::erase_if(sections, [](const testkit::ImageSection &s) {
    return s.meta.type == Type::PriceOnlyNodes;
  });
  auto old = FitArchive::open_copy(join_image(sections));
  ASSERT_TRUE(old.has_value()) << to_string(old.error().status);
  auto current = FitArchive::open_copy(sample_image());
  ASSERT_TRUE(current.has_value());
  ASSERT_EQ(old->fits().size(), current->fits().size());
  for (std::size_t i = 0; i < old->fits().size(); ++i) {
    EXPECT_EQ(std::memcmp(&old->fits()[i], &current->fits()[i], 896), 0) << i;
    auto x = decode_fit(*old, old->fits()[i]);
    ASSERT_TRUE(x.has_value());
    EXPECT_EQ(x->floor_policy, QuoteSliceFloorPolicy::Legacy);
    EXPECT_TRUE(x->price_only_nodes.empty());
    EXPECT_EQ(x->snapshot.spot_version, 0u);
    EXPECT_TRUE(std::isnan(x->snapshot.raw_spot));
    for (const QuoteSliceRow &row : x->rows) {
      EXPECT_EQ(row.price_only_reason, QuoteSlicePriceOnlyReason::None);
      EXPECT_EQ(row.vol_source, QuoteSliceVolConvention::None);
      EXPECT_TRUE(std::isnan(row.iv_lower) && std::isnan(row.iv_upper));
    }
  }
}

// A minor-1 tail that breaks its own invariants is refused, never misread.
TEST(FitArchiveEvolution, InvalidIdentifiabilityTailIsRefused) {
  const auto planted = [](Type type, std::size_t offset, std::uint8_t value) {
    auto sections = split_image(sample_image());
    auto *s = find_section(sections, type);
    EXPECT_NE(s, nullptr);
    if (s != nullptr) s->bytes[offset] = std::byte{value};
    return open_status(join_image(sections), FitArchiveVerify::Paranoid);
  };
  EXPECT_EQ(planted(Type::QuoteRows, offsetof(FitArchiveQuoteRow, price_only_reason), 9),
            ArchiveStatus::RecordInvalid);
  EXPECT_EQ(planted(Type::QuoteRows, offsetof(FitArchiveQuoteRow, iv_upper), 1),
            ArchiveStatus::RecordInvalid); // an interval without its flag
  EXPECT_EQ(planted(Type::FitRecords, offsetof(FitArchiveFitRecord, floor_policy), 2),
            ArchiveStatus::RecordInvalid);
  EXPECT_EQ(planted(Type::FitRecords, offsetof(FitArchiveFitRecord, raw_spot) + 7, 0x40),
            ArchiveStatus::RecordInvalid); // a raw spot without its flag
}

TEST(FitArchiveEvolution, StrideBelowMinorZeroAndNonEvolvableStridesRefused) {
  auto narrow = split_image(sample_image());
  auto *fits = find_section(narrow, Type::FitRecords);
  std::vector<std::byte> shorter;
  for (std::size_t at = 0; at < fits->bytes.size(); at += sizeof(FitArchiveFitRecord)) {
    shorter.insert(shorter.end(), fits->bytes.begin() + static_cast<std::ptrdiff_t>(at),
                   fits->bytes.begin() + static_cast<std::ptrdiff_t>(at + 888));
  }
  fits->bytes = std::move(shorter);
  fits->meta.element_stride = 888;
  EXPECT_EQ(open_status(join_image(narrow)), ArchiveStatus::SectionStride);

  auto nodes = split_image(sample_image());
  auto *price = find_section(nodes, Type::PriceNodes);
  ASSERT_NE(price, nullptr);
  price->bytes.resize(price->bytes.size() / 24 * 32);
  price->meta.element_stride = 32;
  EXPECT_EQ(open_status(join_image(nodes)), ArchiveStatus::SectionStride);
}

} // namespace
} // namespace atx::vol
