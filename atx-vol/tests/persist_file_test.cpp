// persist_file_test.cpp — the PersistFile (ATXVPF) container and the PersistDirectory
// manifest pattern (api/persist/persist_format.hpp, persist_file.hpp; design
// wave3-persist D2-D9, spec docs/storage/PERSIST_FORMAT.md).
//
// Every refusal class of the reader is pinned by a crafted image (a valid image,
// mutated, then re-sealed where the test wants the mutation to survive the checksum
// that would otherwise catch it first). PersistFile.NoAllocationAfterOpen lives in
// persist_alloc_test.cpp (a focused exe: it replaces global operator new).

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "atx/vol/api/persist/persist_file.hpp"
#include "atx/vol/api/persist/persist_format.hpp"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace atx::vol; // NOLINT(google-build-using-namespace): test file

constexpr std::uint64_t kTestFp = 0x5EC7105EC7105EC7ull;
constexpr PersistKind kTestKind = PersistKind::RiskRun;

[[nodiscard]] fs::path fresh_dir(std::string_view tag) {
  const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string name = "atx-persist-" + std::string(tag);
  if (info != nullptr) name += std::string("-") + info->test_suite_name() + "-" + info->name();
#if defined(_WIN32)
  name += "-" + std::to_string(_getpid());
#else
  name += "-" + std::to_string(getpid());
#endif
  const fs::path dir = fs::temp_directory_path() / name;
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  return dir;
}

template <class T> [[nodiscard]] std::span<const std::byte> as_bytes_of(const std::vector<T> &v) {
  return std::as_bytes(std::span<const T>{v});
}

// The owned columns behind sample_inputs(); kept alive by the caller.
struct SampleData {
  struct Pair {
    std::uint64_t a;
    std::uint64_t b;
  };
  std::vector<Pair> pod{{1, 2}, {3, 4}, {5, 6}};
  std::vector<double> f64;
  std::vector<std::int64_t> i64;
  std::vector<std::uint8_t> u8;
  std::string blob{"AAPL MSFT SPY"};
  std::vector<double> f32_origin; // doubles with zero low mantissa bytes (vendor-style)
  SampleData() {
    std::mt19937_64 rng(7);
    std::uniform_real_distribution<double> u(-3.0, 3.0);
    for (int i = 0; i < 100; ++i) {
      f64.push_back(u(rng));
      i64.push_back(static_cast<std::int64_t>(rng()));
      u8.push_back(static_cast<std::uint8_t>(i % 2));
    }
    for (int i = 0; i < 4096; ++i) {
      f32_origin.push_back(static_cast<double>(static_cast<float>(100.0 + 0.25 * (i % 97))));
    }
  }
};

[[nodiscard]] std::vector<PersistSectionInput> sample_inputs(const SampleData &d,
                                                            bool with_codec = true) {
  std::vector<PersistSectionInput> in;
  in.push_back({1, 1, 0, PersistDType::Pod, 16, true, {}, {}, 3, as_bytes_of(d.pod)});
  in.push_back({3, 1, 0, PersistDType::Bytes, 1, true, {}, {}, 3,
                std::as_bytes(std::span<const char>{d.blob.data(), d.blob.size()})});
  in.push_back({16, 1, 0, PersistDType::F64, 8, true, {}, {}, 3, as_bytes_of(d.f64)});
  in.push_back({17, 1, 0, PersistDType::I64, 8, true, {}, {}, 3, as_bytes_of(d.i64)});
  in.push_back({18, 1, 0, PersistDType::U8, 1, false, {}, {}, 3, as_bytes_of(d.u8)});
  if (with_codec) {
    in.push_back({40, 1, 0, PersistDType::F64, 8, false, PersistCodec::Zstd,
                  PersistFilter::ByteShuffle, 3, as_bytes_of(d.f32_origin)});
  }
  return in;
}

[[nodiscard]] PersistFileInfo test_info() {
  PersistFileInfo info;
  info.kind = kTestKind;
  info.schema_fingerprint = kTestFp;
  info.created_unix_ns = 1792000000000000000;
  info.ts_min_ns = 10;
  info.ts_max_ns = 20;
  info.source_fingerprint = 0xABCDu;
  info.writer = "persist-test";
  return info;
}

[[nodiscard]] std::vector<std::byte> sample_image(const SampleData &d, bool with_codec = true) {
  const auto inputs = sample_inputs(d, with_codec);
  auto image = build_persist_file(inputs, test_info());
  EXPECT_TRUE(image.has_value()) << (image ? "" : to_string(image.error().status));
  return image ? std::move(*image) : std::vector<std::byte>{};
}

// 8-byte aligned owned copy (open_bytes refuses a misaligned span).
struct Owned {
  std::shared_ptr<std::vector<std::uint64_t>> words;
  std::span<const std::byte> bytes;
};
[[nodiscard]] Owned own(std::span<const std::byte> image) {
  Owned o;
  o.words = std::make_shared<std::vector<std::uint64_t>>((image.size() + 7u) / 8u, 0u);
  if (!image.empty()) std::memcpy(o.words->data(), image.data(), image.size());
  o.bytes = std::as_bytes(std::span<const std::uint64_t>{*o.words}).first(image.size());
  return o;
}

[[nodiscard]] PersistResult<PersistFile> open_image(std::span<const std::byte> image,
                                                    PersistVerify verify = PersistVerify::Full,
                                                    PersistKind kind = kTestKind,
                                                    std::uint64_t fp = kTestFp) {
  Owned o = own(image);
  return PersistFile::open_bytes(o.bytes, o.words, kind, fp, verify);
}

// ── Image surgery ────────────────────────────────────────────────────────────

template <class T> [[nodiscard]] T load(const std::vector<std::byte> &img, std::size_t at) {
  T v{};
  std::memcpy(&v, img.data() + at, sizeof v);
  return v;
}
template <class T> void store(std::vector<std::byte> &img, std::size_t at, const T &v) {
  std::memcpy(img.data() + at, &v, sizeof v);
}
[[nodiscard]] PersistHeader header_of(const std::vector<std::byte> &img) {
  return load<PersistHeader>(img, 0);
}
[[nodiscard]] std::size_t footer_at(const std::vector<std::byte> &img) {
  return img.size() - sizeof(PersistFooter);
}
[[nodiscard]] std::size_t entry_at(std::size_t i) { return 256 + 64 * i; }
[[nodiscard]] PersistSectionEntry entry_of(const std::vector<std::byte> &img, std::size_t i) {
  return load<PersistSectionEntry>(img, entry_at(i));
}
[[nodiscard]] std::size_t index_of_type(const std::vector<std::byte> &img, std::uint32_t type) {
  const PersistHeader h = header_of(img);
  for (std::size_t i = 0; i < h.section_count; ++i) {
    if (entry_of(img, i).type == type) return i;
  }
  ADD_FAILURE() << "no section " << type;
  return 0;
}

// Recompute table CRC -> header CRC -> footer copies -> footer CRC (and the whole-file
// CRC), so a mutation survives every checksum that guards it.
void reseal(std::vector<std::byte> &img) {
  PersistHeader h = header_of(img);
  const std::size_t table = std::size_t{h.section_count} * sizeof(PersistSectionEntry);
  h.section_table_crc32c = persist_crc32c(std::span<const std::byte>{img.data() + 256, table});
  h.header_crc32c = 0;
  h.header_crc32c = persist_crc32c(std::as_bytes(std::span{&h, 1}));
  store(img, 0, h);
  PersistFooter f = load<PersistFooter>(img, footer_at(img));
  f.header_crc32c = h.header_crc32c;
  f.file_crc32c = persist_crc32c(std::span<const std::byte>{img.data(), footer_at(img)});
  f.footer_crc32c = 0;
  f.footer_crc32c = persist_crc32c(std::as_bytes(std::span{&f, 1}));
  store(img, footer_at(img), f);
}

void mutate_header(std::vector<std::byte> &img, const std::function<void(PersistHeader &)> &fn) {
  PersistHeader h = header_of(img);
  fn(h);
  store(img, 0, h);
  reseal(img);
}
void mutate_entry(std::vector<std::byte> &img, std::size_t i,
                  const std::function<void(PersistSectionEntry &)> &fn) {
  PersistSectionEntry e = entry_of(img, i);
  fn(e);
  store(img, entry_at(i), e);
  reseal(img);
}
void mutate_footer(std::vector<std::byte> &img, const std::function<void(PersistFooter &)> &fn) {
  PersistFooter f = load<PersistFooter>(img, footer_at(img));
  fn(f);
  f.footer_crc32c = 0;
  f.footer_crc32c = persist_crc32c(std::as_bytes(std::span{&f, 1}));
  store(img, footer_at(img), f);
}

} // namespace

// ── PersistFile ─────────────────────────────────────────────────────────────

TEST(PersistFile, Crc32cKnownAnswer) {
  // The CRC-32C check value (RFC 3720 / iSCSI): "123456789" -> 0xE3069283.
  const std::string_view nine = "123456789";
  EXPECT_EQ(persist_crc32c(std::as_bytes(std::span{nine.data(), nine.size()})), 0xE3069283u);
  EXPECT_EQ(persist_crc32c({}), 0u);
  // 32 zero bytes (RFC 3720 B.4): 0x8A9136AA.
  const std::array<std::byte, 32> zeros{};
  EXPECT_EQ(persist_crc32c(zeros), 0x8A9136AAu);
}

TEST(PersistFile, FingerprintKnownAnswer) {
  // Pinned against an independent Python implementation of the same serialization
  // (FNV-1a/64 over little-endian fields, length-prefixed names, then fmix64).
  static constexpr PersistFieldSpec kCarry[] = {
      {"expiry_ns", 0, 8}, {"pv_divs", 8, 8}, {"rate", 16, 8}, {"q", 24, 8}};
  static constexpr PersistSectionSpec kSchema[] = {
      {1, 1, PersistDType::Pod, 32, "carry", kCarry, true},
      {16, 1, PersistDType::F64, 8, "strike", {}, true},
      {18, 2, PersistDType::U8, 1, "side", {}, false},
  };
  static_assert(persist_salt("ATXVKA01") == 0x3130414b56585441ull);
  static_assert(persist_fingerprint(persist_salt("ATXVKA01"), kSchema) == 0xafc0b50aa5cf0f7aull);
  static_assert(persist_fields_tile(kCarry, 32));
  EXPECT_EQ(persist_manifest_fingerprint(), 0x9a77019356eecf98ull);
  // Any schema edit moves it.
  static constexpr PersistSectionSpec kRenamed[] = {
      {1, 1, PersistDType::Pod, 32, "carry", kCarry, true},
      {16, 1, PersistDType::F64, 8, "strikes", {}, true},
      {18, 2, PersistDType::U8, 1, "side", {}, false},
  };
  static constexpr PersistSectionSpec kOptional[] = {
      {1, 1, PersistDType::Pod, 32, "carry", kCarry, true},
      {16, 1, PersistDType::F64, 8, "strike", {}, true},
      {18, 2, PersistDType::U8, 1, "side", {}, true},
  };
  EXPECT_NE(persist_fingerprint(persist_salt("ATXVKA01"), kRenamed), 0xafc0b50aa5cf0f7aull);
  EXPECT_NE(persist_fingerprint(persist_salt("ATXVKA01"), kOptional), 0xafc0b50aa5cf0f7aull);
}

TEST(PersistFile, RoundTripsSectionsBitExact) {
  const SampleData d;
  const auto inputs = sample_inputs(d);
  const std::vector<std::byte> image = sample_image(d);
  ASSERT_FALSE(image.empty());
  for (const PersistVerify v : {PersistVerify::Framing, PersistVerify::Lazy, PersistVerify::Full,
                                PersistVerify::Paranoid}) {
    auto f = open_image(image, v);
    ASSERT_TRUE(f.has_value()) << to_string(f.error().status);
    EXPECT_EQ(f->header().kind, static_cast<std::uint32_t>(kTestKind));
    EXPECT_EQ(f->header().schema_fingerprint, kTestFp);
    EXPECT_EQ(f->header().source_fingerprint, 0xABCDu);
    EXPECT_EQ(f->header().created_unix_ns, 1792000000000000000);
    EXPECT_EQ(f->header().ts_min_ns, 10);
    EXPECT_EQ(f->header().ts_max_ns, 20);
    EXPECT_EQ(std::string_view(f->header().writer_name.data()), "persist-test");
    ASSERT_EQ(f->sections().size(), inputs.size());
    for (const PersistSectionInput &in : inputs) {
      const PersistSectionEntry *e = f->entry(in.type);
      ASSERT_NE(e, nullptr);
      EXPECT_EQ(e->dtype, static_cast<std::uint8_t>(in.dtype));
      EXPECT_EQ(e->element_stride, in.stride);
      EXPECT_EQ(e->raw_length, in.bytes.size());
      EXPECT_EQ(e->flags, in.required ? kPersistSectionRequired : 0u);
      auto got = f->section(in.type);
      ASSERT_TRUE(got.has_value()) << in.type;
      ASSERT_EQ(got->size(), in.bytes.size());
      EXPECT_EQ(std::memcmp(got->data(), in.bytes.data(), in.bytes.size()), 0) << in.type;
    }
    auto f64 = f->typed<double>(16);
    ASSERT_TRUE(f64.has_value());
    ASSERT_EQ(f64->size(), d.f64.size());
    for (std::size_t i = 0; i < d.f64.size(); ++i) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>((*f64)[i]), std::bit_cast<std::uint64_t>(d.f64[i]));
    }
    auto pairs = f->typed<SampleData::Pair>(1);
    ASSERT_TRUE(pairs.has_value());
    EXPECT_EQ((*pairs)[2].b, 6u);
    EXPECT_EQ(f->typed<std::int64_t>(16).error().status, PersistStatus::SectionStride);
    EXPECT_EQ(f->typed<double>(99).error().status, PersistStatus::MissingSection);
    EXPECT_EQ(f->section(99).error().status, PersistStatus::MissingSection);
    EXPECT_TRUE(f->verify_all().has_value());
  }
  // File round trip, mapped and read.
  const fs::path dir = fresh_dir("rt");
  const fs::path path = dir / "a.atxrr";
  ASSERT_TRUE(publish_persist_file(path.string(), image).has_value());
  for (const bool mapped : {true, false}) {
    PersistOpenOptions o;
    o.mapped = mapped;
    o.verify = PersistVerify::Paranoid;
    o.prefetch = mapped;
    o.expected_size = image.size();
    auto f = PersistFile::open(path.string(), kTestKind, kTestFp, o);
    ASSERT_TRUE(f.has_value()) << to_string(f.error().status);
    EXPECT_EQ(f->bytes().size(), image.size());
    EXPECT_EQ(std::memcmp(f->bytes().data(), image.data(), image.size()), 0);
    EXPECT_NE(f->keepalive(), nullptr);
  }
  PersistOpenOptions wrong;
  wrong.expected_size = image.size() + 4096;
  EXPECT_EQ(PersistFile::open(path.string(), kTestKind, kTestFp, wrong).error().status,
            PersistStatus::SizeMismatch);
  EXPECT_EQ(PersistFile::open((dir / "absent.atxrr").string(), kTestKind, kTestFp).error().status,
            PersistStatus::NotFound);
  // The bridge into house Status.
  const Status st = to_status(PersistError{PersistStatus::SectionChecksum, 16, 3});
  ASSERT_FALSE(st.has_value());
  EXPECT_NE(std::string(st.error().message()).find("section-checksum"), std::string::npos);
  EXPECT_EQ(to_status(PersistError{PersistStatus::NotFound, kPersistNone, 0}).error().code(),
            ErrorCode::NotFound);
}

TEST(PersistFile, SectionsAre64ByteAlignedAndSizeIsPageMultiple) {
  const SampleData d;
  const std::vector<std::byte> image = sample_image(d);
  ASSERT_FALSE(image.empty());
  EXPECT_EQ(image.size() % kPersistFileAlign, 0u);
  const PersistHeader h = header_of(image);
  EXPECT_EQ(h.file_size, image.size());
  EXPECT_EQ(h.section_table_offset, 256u);
  std::uint64_t previous_end = 256u + 64u * h.section_count;
  for (std::size_t i = 0; i < h.section_count; ++i) {
    const PersistSectionEntry e = entry_of(image, i);
    EXPECT_EQ(e.offset % kPersistSectionAlign, 0u) << i;
    EXPECT_GE(e.offset, previous_end);
    previous_end = e.offset + e.length;
  }
  EXPECT_LE(previous_end, image.size() - 64u);
  const PersistFooter f = load<PersistFooter>(image, footer_at(image));
  EXPECT_EQ(f.magic, kPersistFooterMagic);
  EXPECT_EQ(f.file_size, image.size());
  // Empty section list: header + footer padded to one page.
  auto empty = build_persist_file({}, test_info());
  ASSERT_TRUE(empty.has_value());
  EXPECT_EQ(empty->size(), kPersistFileAlign);
  EXPECT_TRUE(open_image(*empty, PersistVerify::Paranoid).has_value());
}

TEST(PersistFile, DeterministicBytesForPinnedInfo) {
  const SampleData d;
  const auto inputs = sample_inputs(d);
  auto a = build_persist_file(inputs, test_info(), 1);
  auto b = build_persist_file(inputs, test_info(), 1);
  auto c = build_persist_file(inputs, test_info(), 8);
  ASSERT_TRUE(a && b && c);
  EXPECT_EQ(*a, *b);
  EXPECT_EQ(*a, *c) << "codec fan-out must not change the bytes";
  // created_unix_ns is the only wall-clock input, and it is the caller's.
  PersistFileInfo other = test_info();
  other.created_unix_ns += 1;
  auto e = build_persist_file(inputs, other, 1);
  ASSERT_TRUE(e.has_value());
  ASSERT_EQ(e->size(), a->size());
  EXPECT_NE(*e, *a);
  std::size_t first_diff = 0;
  while (first_diff < a->size() && (*a)[first_diff] == (*e)[first_diff]) ++first_diff;
  EXPECT_EQ(first_diff, offsetof(PersistHeader, created_unix_ns));
}

namespace {

struct CraftCase {
  const char *name;
  std::function<void(std::vector<std::byte> &)> craft;
  PersistVerify verify;
  PersistStatus want;
};

using Img = std::vector<std::byte>;

[[nodiscard]] std::vector<CraftCase> craft_cases() {
  using V = PersistVerify;
  using S = PersistStatus;
  std::vector<CraftCase> c;
  c.push_back({"empty", [](Img &i) { i.clear(); }, V::Full, S::Truncated});
  c.push_back({"short-foreign", [](Img &i) { i.assign(100, std::byte{'X'}); }, V::Full,
               S::BadMagic});
  c.push_back({"short-own-magic", [](Img &i) { i.resize(200); }, V::Full, S::Truncated});
  c.push_back({"bad-magic", [](Img &i) { i[1] = std::byte{'W'}; }, V::Full, S::BadMagic});
  c.push_back({"endian", [](Img &i) {
                 mutate_header(i, [](PersistHeader &h) { h.endian_tag = 0x04030201u; });
               }, V::Full, S::UnsupportedEndian});
  c.push_back({"major", [](Img &i) {
                 mutate_header(i, [](PersistHeader &h) { h.format_major = 2; });
               }, V::Full, S::UnsupportedMajor});
  c.push_back({"header-size", [](Img &i) {
                 mutate_header(i, [](PersistHeader &h) { h.header_size = 128; });
               }, V::Full, S::HeaderInvalid});
  c.push_back({"header-crc", [](Img &i) { i[100] = std::byte{'#'}; }, V::Full, S::HeaderChecksum});
  c.push_back({"header-reserved", [](Img &i) {
                 mutate_header(i, [](PersistHeader &h) { h.reserved[7] = 1; });
               }, V::Full, S::HeaderInvalid});
  c.push_back({"header-flags", [](Img &i) {
                 mutate_header(i, [](PersistHeader &h) { h.flags = 2; });
               }, V::Full, S::HeaderInvalid});
  c.push_back({"file-size-unaligned", [](Img &i) {
                 mutate_header(i, [](PersistHeader &h) { h.file_size -= 64; });
               }, V::Full, S::HeaderInvalid});
  c.push_back({"truncated", [](Img &i) { i.resize(i.size() - 4096); }, V::Full, S::Truncated});
  c.push_back({"extended", [](Img &i) { i.resize(i.size() + 4096); }, V::Full, S::SizeMismatch});
  c.push_back({"wrong-kind", [](Img &i) {
                 mutate_header(i, [](PersistHeader &h) { h.kind = 2; });
               }, V::Full, S::WrongKind});
  c.push_back({"wrong-schema", [](Img &i) {
                 mutate_header(i, [](PersistHeader &h) { h.schema_fingerprint ^= 1u; });
               }, V::Full, S::SchemaMismatch});
  c.push_back({"footer-magic", [](Img &i) { i[footer_at(i) + 2] = std::byte{'X'}; }, V::Full,
               S::FooterInvalid});
  c.push_back({"footer-crc", [](Img &i) { i[footer_at(i) + 44] ^= std::byte{1}; }, V::Full,
               S::FooterChecksum});
  c.push_back({"footer-reserved", [](Img &i) {
                 mutate_footer(i, [](PersistFooter &f) { f.reserved1 = 9; });
               }, V::Full, S::FooterInvalid});
  c.push_back({"footer-mismatch", [](Img &i) {
                 mutate_footer(i, [](PersistFooter &f) { f.section_count += 1; });
               }, V::Full, S::FooterMismatch});
  c.push_back({"table-crc", [](Img &i) { i[entry_at(1) + 24] ^= std::byte{1}; }, V::Full,
               S::SectionTableChecksum});
  c.push_back({"entry-reserved", [](Img &i) {
                 mutate_entry(i, 0, [](PersistSectionEntry &e) { e.reserved1 = 1; });
               }, V::Full, S::HeaderInvalid});
  c.push_back({"entry-unaligned", [](Img &i) {
                 mutate_entry(i, 1, [](PersistSectionEntry &e) { e.offset += 8; });
               }, V::Full, S::SectionAlignment});
  c.push_back({"entry-out-of-bounds", [](Img &i) {
                 const std::uint64_t past = i.size();
                 mutate_entry(i, 2, [&](PersistSectionEntry &e) { e.offset = past; });
               }, V::Full, S::SectionBounds});
  c.push_back({"entry-overlap", [](Img &i) {
                 const std::uint64_t first = entry_of(i, 0).offset;
                 mutate_entry(i, 1, [&](PersistSectionEntry &e) { e.offset = first; });
               }, V::Full, S::SectionOverlap});
  c.push_back({"entry-length-not-stride", [](Img &i) {
                 mutate_entry(i, 2, [](PersistSectionEntry &e) {
                   e.length -= 4;
                   e.raw_length -= 4;
                 });
               }, V::Full, S::SectionStride});
  c.push_back({"entry-count", [](Img &i) {
                 mutate_entry(i, 2, [](PersistSectionEntry &e) { e.element_count += 1; });
               }, V::Full, S::SectionStride});
  c.push_back({"scalar-stride", [](Img &i) {
                 mutate_entry(i, 2, [](PersistSectionEntry &e) {
                   e.element_stride = 4;
                   e.element_count *= 2;
                 });
               }, V::Full, S::SectionStride});
  c.push_back({"raw-length-without-codec", [](Img &i) {
                 mutate_entry(i, 3, [](PersistSectionEntry &e) { e.raw_length += 8; });
               }, V::Full, S::SectionStride});
  c.push_back({"duplicate-type", [](Img &i) {
                 mutate_entry(i, 3, [](PersistSectionEntry &e) { e.type = 16; });
               }, V::Full, S::DuplicateSection});
  c.push_back({"unknown-dtype", [](Img &i) {
                 mutate_entry(i, 1, [](PersistSectionEntry &e) { e.dtype = 99; });
               }, V::Full, S::UnsupportedSection});
  c.push_back({"unknown-codec", [](Img &i) {
                 mutate_entry(i, 5, [](PersistSectionEntry &e) { e.codec = 7; });
               }, V::Full, S::UnsupportedSection});
  c.push_back({"unknown-flag", [](Img &i) {
                 mutate_entry(i, 0, [](PersistSectionEntry &e) { e.flags |= 4u; });
               }, V::Full, S::UnsupportedSection});
  c.push_back({"payload-crc", [](Img &i) {
                 i[entry_of(i, 2).offset + 3] ^= std::byte{0x10};
               }, V::Full, S::SectionChecksum});
  c.push_back({"padding", [](Img &i) {
                 const PersistSectionEntry e = entry_of(i, 0);
                 i[e.offset + e.length + 1] = std::byte{1};
               }, V::Full, S::PaddingNotZero});
  c.push_back({"file-crc", [](Img &i) {
                 mutate_footer(i, [](PersistFooter &f) { f.file_crc32c ^= 1u; });
               }, V::Paranoid, S::FileChecksum});
  c.push_back({"codec-stored-crc", [](Img &i) {
                 i[entry_of(i, 5).offset + 5] ^= std::byte{0x40};
               }, V::Lazy, S::SectionChecksum});
  c.push_back({"codec-garbage-frame", [](Img &i) {
                 const PersistSectionEntry e = entry_of(i, 5);
                 for (std::uint64_t k = 0; k < e.length; ++k) i[e.offset + k] = std::byte{0x5A};
                 const std::uint32_t crc = persist_crc32c(
                     std::span<const std::byte>{i.data() + e.offset, static_cast<std::size_t>(e.length)});
                 mutate_entry(i, 5, [&](PersistSectionEntry &x) { x.crc32c = crc; });
               }, V::Framing, S::CodecError});
  c.push_back({"codec-raw-crc", [](Img &i) {
                 mutate_entry(i, 5, [](PersistSectionEntry &e) { e.raw_crc32c ^= 1u; });
               }, V::Framing, S::RawChecksum});
  c.push_back({"codec-raw-length", [](Img &i) {
                 mutate_entry(i, 5, [](PersistSectionEntry &e) {
                   e.raw_length += 8;
                   e.element_count += 1;
                 });
               }, V::Framing, S::CodecError});
  c.push_back({"filter-without-codec", [](Img &i) {
                 mutate_entry(i, 2, [](PersistSectionEntry &e) { e.filter = 1; });
               }, V::Full, S::HeaderInvalid});
  return c;
}

} // namespace

TEST(PersistFile, RefusesEachCraftedCorruption) {
  const SampleData d;
  const std::vector<std::byte> good = sample_image(d);
  ASSERT_FALSE(good.empty());
  ASSERT_EQ(entry_of(good, 5).type, 40u) << "case table indexes the sample's section order";
  const std::vector<CraftCase> cases = craft_cases();
  ASSERT_GE(cases.size(), 25u);
  for (const CraftCase &c : cases) {
    std::vector<std::byte> img = good;
    c.craft(img);
    auto f = open_image(img, c.verify);
    ASSERT_FALSE(f.has_value()) << c.name << " was accepted";
    EXPECT_EQ(f.error().status, c.want)
        << c.name << ": got " << to_string(f.error().status) << " want " << to_string(c.want);
  }
  // Misaligned backing.
  std::vector<std::uint64_t> words((good.size() + 16u) / 8u);
  auto *base = std::as_writable_bytes(std::span{words}).data();
  std::memcpy(base + 1, good.data(), good.size());
  const std::span<const std::byte> odd{base + 1, good.size()};
  EXPECT_EQ(PersistFile::open_bytes(odd, nullptr, kTestKind, kTestFp, PersistVerify::Full)
                .error()
                .status,
            PersistStatus::Misaligned);
  EXPECT_TRUE(open_image(good, PersistVerify::Paranoid).has_value());
}

TEST(PersistFile, RefusesWrongKindAndSchema) {
  const SampleData d;
  const std::vector<std::byte> image = sample_image(d);
  EXPECT_EQ(open_image(image, PersistVerify::Full, PersistKind::MarketState).error().status,
            PersistStatus::WrongKind);
  EXPECT_EQ(open_image(image, PersistVerify::Full, kTestKind, kTestFp + 1).error().status,
            PersistStatus::SchemaMismatch);
  // A damaged kind field is a checksum failure, never a confident WrongKind.
  std::vector<std::byte> damaged = image;
  damaged[offsetof(PersistHeader, kind)] ^= std::byte{1};
  EXPECT_EQ(open_image(damaged).error().status, PersistStatus::HeaderChecksum);
  EXPECT_TRUE(open_image(image).has_value());
}

TEST(PersistFile, SkipsUnknownOptionalRefusesUnknownRequired) {
  const SampleData d;
  auto inputs = sample_inputs(d, false);
  const std::string extra = "future";
  inputs.push_back({99, 1, 0, PersistDType::Bytes, 1, false, {}, {}, 3,
                    std::as_bytes(std::span<const char>{extra.data(), extra.size()})});
  auto image = build_persist_file(inputs, test_info());
  ASSERT_TRUE(image.has_value());
  auto f = open_image(*image);
  ASSERT_TRUE(f.has_value());

  static constexpr PersistFieldSpec kPair[] = {{"a", 0, 8}, {"b", 8, 8}};
  const PersistSectionSpec known[] = {
      {1, 1, PersistDType::Pod, 16, "pairs", kPair, true},
      {3, 1, PersistDType::Bytes, 1, "strings", {}, true},
      {16, 1, PersistDType::F64, 8, "f64", {}, true},
      {17, 1, PersistDType::I64, 8, "i64", {}, true},
      {18, 1, PersistDType::U8, 1, "u8", {}, false},
      {20, 1, PersistDType::F64, 8, "absent-optional", {}, false},
  };
  EXPECT_TRUE(f->check_schema(known).has_value()) << "unknown optional 99 must be skipped";

  const PersistSectionSpec needs_absent[] = {{1, 1, PersistDType::Pod, 16, "pairs", kPair, true},
                                             {21, 1, PersistDType::F64, 8, "absent", {}, true}};
  auto missing = f->check_schema(needs_absent);
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().status, PersistStatus::MissingSection);
  EXPECT_EQ(missing.error().section, 21u);

  const PersistSectionSpec major2[] = {{16, 2, PersistDType::F64, 8, "f64", {}, true}};
  EXPECT_EQ(f->check_schema(major2).error().status, PersistStatus::UnsupportedSectionMajor);
  const PersistSectionSpec retyped[] = {{16, 1, PersistDType::I64, 8, "f64", {}, true}};
  EXPECT_EQ(f->check_schema(retyped).error().status, PersistStatus::SectionStride);

  // The same unknown type flagged Required is refused.
  inputs.back().required = true;
  auto strict = build_persist_file(inputs, test_info());
  ASSERT_TRUE(strict.has_value());
  auto g = open_image(*strict);
  ASSERT_TRUE(g.has_value());
  auto refused = g->check_schema(known);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().status, PersistStatus::UnsupportedSection);
  EXPECT_EQ(refused.error().section, 99u);
}

TEST(PersistFile, LazyVerifyReportsChecksumOnFirstTouch) {
  const SampleData d;
  std::vector<std::byte> image = sample_image(d);
  const PersistSectionEntry bad = entry_of(image, index_of_type(image, 16));
  image[bad.offset + 9] ^= std::byte{0x01};

  auto lazy = open_image(image, PersistVerify::Lazy);
  ASSERT_TRUE(lazy.has_value()) << "Lazy defers section CRCs to the first touch";
  EXPECT_TRUE(lazy->section(17).has_value());
  for (int touch = 0; touch < 3; ++touch) {
    auto s = lazy->section(16);
    ASSERT_FALSE(s.has_value());
    EXPECT_EQ(s.error().status, PersistStatus::SectionChecksum);
    EXPECT_EQ(s.error().section, 16u);
  }
  EXPECT_EQ(lazy->typed<double>(16).error().status, PersistStatus::SectionChecksum);
  EXPECT_TRUE(lazy->section(18).has_value());
  EXPECT_EQ(lazy->verify_all().error().status, PersistStatus::SectionChecksum);

  auto framing = open_image(image, PersistVerify::Framing);
  ASSERT_TRUE(framing.has_value());
  EXPECT_TRUE(framing->section(16).has_value()) << "Framing never checks payload CRCs on touch";
  EXPECT_EQ(framing->verify_all().error().status, PersistStatus::SectionChecksum);

  EXPECT_EQ(open_image(image, PersistVerify::Full).error().status, PersistStatus::SectionChecksum);
}

TEST(PersistFile, ConcurrentLazyVerifyIsRaceFree) {
  // 32 sections, every fourth one damaged; 8 threads race their first touches.
  std::vector<std::vector<double>> cols(32);
  std::vector<PersistSectionInput> inputs;
  for (std::uint32_t t = 0; t < 32; ++t) {
    cols[t].assign(2048, 0.5 * t);
    inputs.push_back({100 + t, 1, 0, PersistDType::F64, 8, true, {}, {}, 3, as_bytes_of(cols[t])});
  }
  auto built = build_persist_file(inputs, test_info());
  ASSERT_TRUE(built.has_value());
  std::vector<std::byte> image = *built;
  for (std::uint32_t t = 0; t < 32; t += 4) {
    image[entry_of(image, t).offset + 17] ^= std::byte{0x80};
  }
  constexpr int kThreads = 8;
  for (int round = 0; round < 25; ++round) {
    auto f = open_image(image, PersistVerify::Lazy);
    ASSERT_TRUE(f.has_value());
    std::vector<std::array<std::uint8_t, 32>> verdicts(kThreads);
    std::atomic<int> ready{0};
    std::vector<std::thread> pool;
    for (int w = 0; w < kThreads; ++w) {
      pool.emplace_back([&, w] {
        ready.fetch_add(1);
        while (ready.load() < kThreads) {
        }
        for (std::uint32_t k = 0; k < 32; ++k) {
          const std::uint32_t t = (k * 7u + static_cast<std::uint32_t>(w) * 5u) % 32u;
          verdicts[static_cast<std::size_t>(w)][t] = f->section(100 + t).has_value() ? 1 : 0;
        }
      });
    }
    for (std::thread &t : pool) t.join();
    for (int w = 0; w < kThreads; ++w) {
      for (std::uint32_t t = 0; t < 32; ++t) {
        EXPECT_EQ(verdicts[static_cast<std::size_t>(w)][t], t % 4 == 0 ? 0 : 1)
            << "round " << round << " thread " << w << " section " << t;
      }
    }
  }
}

TEST(PersistFile, ZstdShuffleRoundTripsBitExact) {
  const SampleData d;
  std::vector<double> noise(4096);
  std::mt19937_64 rng(11);
  for (double &x : noise) x = std::bit_cast<double>((rng() >> 2) | 0x3FF0000000000000ull);
  std::vector<std::uint8_t> sides(5000);
  for (std::size_t i = 0; i < sides.size(); ++i) sides[i] = static_cast<std::uint8_t>((i / 50) % 2);
  std::vector<SampleData::Pair> pods(777);
  for (std::size_t i = 0; i < pods.size(); ++i) pods[i] = {i, i * i};
  const std::vector<std::byte> nothing;

  struct Case {
    std::uint32_t type;
    PersistDType dtype;
    std::uint32_t stride;
    PersistFilter filter;
    int level;
    std::span<const std::byte> bytes;
  };
  const Case cases[] = {
      {40, PersistDType::F64, 8, PersistFilter::ByteShuffle, 3, as_bytes_of(d.f32_origin)},
      {41, PersistDType::F64, 8, PersistFilter::None, 3, as_bytes_of(d.f32_origin)},
      {42, PersistDType::F64, 8, PersistFilter::ByteShuffle, 1, as_bytes_of(noise)},
      {43, PersistDType::U8, 1, PersistFilter::ByteShuffle, 3, as_bytes_of(sides)},
      {44, PersistDType::Pod, 16, PersistFilter::ByteShuffle, 19, as_bytes_of(pods)},
      {45, PersistDType::Bytes, 1, PersistFilter::None, 3, nothing},
  };
  std::vector<PersistSectionInput> inputs;
  for (const Case &c : cases) {
    inputs.push_back({c.type, 1, 0, c.dtype, c.stride, true, PersistCodec::Zstd, c.filter, c.level,
                      c.bytes});
  }
  auto image = build_persist_file(inputs, test_info(), 4);
  ASSERT_TRUE(image.has_value()) << to_string(image.error().status);
  for (const PersistVerify v :
       {PersistVerify::Framing, PersistVerify::Lazy, PersistVerify::Paranoid}) {
    auto f = open_image(*image, v);
    ASSERT_TRUE(f.has_value()) << to_string(f.error().status);
    for (const Case &c : cases) {
      const PersistSectionEntry *e = f->entry(c.type);
      ASSERT_NE(e, nullptr);
      EXPECT_EQ(e->codec, static_cast<std::uint8_t>(PersistCodec::Zstd));
      EXPECT_EQ(e->filter, static_cast<std::uint8_t>(c.filter));
      EXPECT_EQ(e->raw_length, c.bytes.size());
      auto got = f->section(c.type);
      ASSERT_TRUE(got.has_value()) << c.type;
      ASSERT_EQ(got->size(), c.bytes.size());
      EXPECT_TRUE(c.bytes.empty() || std::memcmp(got->data(), c.bytes.data(), c.bytes.size()) == 0)
          << c.type;
      if (!got->empty()) {
        // SAFETY: address arithmetic only.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(got->data()) % 64u, 0u)
            << "decoded sections are 64-byte aligned";
      }
    }
    auto typed = f->typed<double>(40);
    ASSERT_TRUE(typed.has_value());
    EXPECT_EQ(std::bit_cast<std::uint64_t>((*typed)[123]),
              std::bit_cast<std::uint64_t>(d.f32_origin[123]));
  }
  // Shuffle exposes the zero low mantissa bytes of f32-origin doubles.
  auto f = open_image(*image);
  ASSERT_TRUE(f.has_value());
  const std::uint64_t shuffled = f->entry(40)->length;
  const std::uint64_t plain = f->entry(41)->length;
  EXPECT_LT(shuffled, plain);
  EXPECT_LT(shuffled * 4, d.f32_origin.size() * sizeof(double));
}

TEST(PersistFile, PublishRefusesExistingPath) {
  const SampleData d;
  const std::vector<std::byte> image = sample_image(d);
  const fs::path dir = fresh_dir("pub");
  const fs::path path = dir / "x.atxrr";
  ASSERT_TRUE(publish_persist_file(path.string(), image).has_value());
  const auto size_before = fs::file_size(path);
  std::vector<std::byte> other = sample_image(d, false);
  auto again = publish_persist_file(path.string(), other);
  ASSERT_FALSE(again.has_value());
  EXPECT_EQ(again.error().status, PersistStatus::AlreadyExists);
  EXPECT_EQ(fs::file_size(path), size_before) << "the published file is never rewritten";
  auto f = PersistFile::open(path.string(), kTestKind, kTestFp,
                             PersistOpenOptions{true, PersistVerify::Paranoid, false, 0});
  ASSERT_TRUE(f.has_value());
  EXPECT_EQ(f->sections().size(), 6u);
  // No temp left behind.
  std::size_t files = 0;
  for (const auto &entry : fs::directory_iterator(dir)) {
    static_cast<void>(entry);
    ++files;
  }
  EXPECT_EQ(files, 1u);
  EXPECT_EQ(publish_persist_file((dir / "no-such-dir" / "y.atxrr").string(), image).error().status,
            PersistStatus::IoError);
}

// ── PersistDirectory ─────────────────────────────────────────────────────────

namespace {

[[nodiscard]] std::vector<std::byte> state_like_image(std::uint64_t source_fp, std::int64_t ts) {
  static const std::vector<double> col(512, 1.25);
  const PersistSectionInput in{16, 1, 0, PersistDType::F64, 8, true, {}, {}, 3, as_bytes_of(col)};
  PersistFileInfo info;
  info.kind = PersistKind::MarketState;
  info.schema_fingerprint = kTestFp;
  info.source_fingerprint = source_fp;
  info.ts_min_ns = ts;
  info.ts_max_ns = ts;
  auto image = build_persist_file(std::span{&in, 1}, info);
  EXPECT_TRUE(image.has_value());
  return image ? *image : std::vector<std::byte>{};
}

[[nodiscard]] std::string name_of(const PersistManifestEntry &e) {
  return std::string(e.file_name.data(),
                     static_cast<std::size_t>(
                         std::find(e.file_name.begin(), e.file_name.end(), '\0') - e.file_name.begin()));
}

} // namespace

TEST(PersistDirectory, PublishesFileThenManifest) {
  const fs::path dir = fresh_dir("dir") / "date=2026-10-14";
  EXPECT_EQ(PersistDirectory::open(dir.string()).error().status, PersistStatus::NotFound);

  auto e1 = PersistDirectory::publish(dir.string(), "1000", state_like_image(11, 100), 512);
  ASSERT_TRUE(e1.has_value()) << to_string(e1.error().status);
  EXPECT_EQ(e1->seq, 1u);
  EXPECT_EQ(name_of(*e1), "bucket_et=1000-g000000000001.atxms");
  EXPECT_EQ(e1->kind, static_cast<std::uint32_t>(PersistKind::MarketState));
  EXPECT_EQ(e1->source_fingerprint, 11u);
  EXPECT_EQ(e1->records, 512u);
  EXPECT_EQ(e1->ts_min_ns, 100);
  EXPECT_TRUE(fs::exists(dir / std::string(kPersistManifestFileName)));
  EXPECT_TRUE(fs::exists(dir / name_of(*e1)));

  auto e2 = PersistDirectory::publish(dir.string(), "1030", state_like_image(12, 200));
  auto e3 = PersistDirectory::publish(dir.string(), "1000", state_like_image(13, 100));
  ASSERT_TRUE(e2 && e3);
  EXPECT_EQ(e2->seq, 2u);
  EXPECT_EQ(e3->seq, 3u);
  EXPECT_EQ(name_of(*e3), "bucket_et=1000-g000000000003.atxms");
  EXPECT_TRUE(fs::exists(dir / name_of(*e1))) << "a superseded generation stays (it may be mapped)";

  auto d = PersistDirectory::open(dir.string());
  ASSERT_TRUE(d.has_value()) << to_string(d.error().status);
  ASSERT_EQ(d->entries().size(), 2u);
  EXPECT_EQ(std::string_view(d->entries()[0].key.data()), "1000");
  EXPECT_EQ(std::string_view(d->entries()[1].key.data()), "1030");
  const PersistManifestEntry *hit = d->find("1000");
  ASSERT_NE(hit, nullptr);
  EXPECT_EQ(hit->seq, 3u);
  EXPECT_EQ(hit->source_fingerprint, 13u);
  EXPECT_EQ(d->find("1100"), nullptr);
  auto f = PersistFile::open(d->path(*hit), PersistKind::MarketState, kTestFp,
                             PersistOpenOptions{true, PersistVerify::Paranoid, false, hit->file_size});
  ASSERT_TRUE(f.has_value());
  EXPECT_EQ(f->footer().file_crc32c, hit->file_crc32c);
  EXPECT_EQ(f->header().header_crc32c, hit->header_crc32c);

  auto names = persist_file_name(PersistKind::RiskRun, "x", 42);
  ASSERT_TRUE(names.has_value());
  EXPECT_EQ(*names, "seg-000000000042.atxrr");
  EXPECT_EQ(*persist_file_name(PersistKind::SurfaceTimeline, "2026-10", 7),
            "2026-10-g000000000007.atxstl");
}

TEST(PersistDirectory, ReaderIgnoresUnlistedFile) {
  const fs::path dir = fresh_dir("orphan");
  ASSERT_TRUE(PersistDirectory::publish(dir.string(), "1000", state_like_image(1, 1)).has_value());
  // A crash between "publish the file" and "publish the manifest" leaves exactly this:
  // a valid data file under a fresh name that no manifest lists.
  const std::vector<std::byte> orphan = state_like_image(2, 2);
  ASSERT_TRUE(publish_persist_file((dir / "bucket_et=1100-g000000000002.atxms").string(), orphan)
                  .has_value());
  auto d = PersistDirectory::open(dir.string());
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->entries().size(), 1u);
  EXPECT_EQ(d->find("1100"), nullptr);
  // The next publish of that key must not trip over the orphan's name.
  auto next = PersistDirectory::publish(dir.string(), "1100", state_like_image(3, 3));
  ASSERT_TRUE(next.has_value()) << to_string(next.error().status);
  EXPECT_EQ(next->source_fingerprint, 3u);
  EXPECT_NE(name_of(*next), "bucket_et=1100-g000000000002.atxms");
}

TEST(PersistDirectory, RefusesTraversalFileName) {
  for (const std::string_view key :
       {std::string_view{".."}, std::string_view{"."}, std::string_view{"../x"},
        std::string_view{"a/b"}, std::string_view{"a\\b"}, std::string_view{"C:"},
        std::string_view{""}, std::string_view{"abcdefghijklmnopq"}}) {
    EXPECT_EQ(persist_file_name(PersistKind::MarketState, key, 1).error().status,
              PersistStatus::InvalidArgument)
        << key;
  }
  EXPECT_EQ(persist_file_name(PersistKind::Manifest, "1000", 1).error().status,
            PersistStatus::InvalidArgument);
  const fs::path dir = fresh_dir("trav");
  EXPECT_EQ(PersistDirectory::publish(dir.string(), "..", state_like_image(1, 1)).error().status,
            PersistStatus::InvalidArgument);

  // A hand-crafted manifest whose entry names a path outside the directory.
  PersistManifestEntry evil{};
  evil.seq = 1;
  const std::string_view bad = "..\\evil.atxms";
  std::copy(bad.begin(), bad.end(), evil.file_name.begin());
  evil.key[0] = '1';
  evil.kind = static_cast<std::uint32_t>(PersistKind::MarketState);
  const PersistSectionInput in{kPersistManifestEntriesSection, 1, 0, PersistDType::Pod, 128, true,
                               {}, {}, 3, std::as_bytes(std::span{&evil, 1})};
  PersistFileInfo info;
  info.kind = PersistKind::Manifest;
  info.schema_fingerprint = persist_manifest_fingerprint();
  auto image = build_persist_file(std::span{&in, 1}, info);
  ASSERT_TRUE(image.has_value());
  ASSERT_TRUE(publish_persist_file((dir / std::string(kPersistManifestFileName)).string(), *image)
                  .has_value());
  auto d = PersistDirectory::open(dir.string());
  ASSERT_FALSE(d.has_value());
  EXPECT_EQ(d.error().status, PersistStatus::RecordInvalid);
}

TEST(PersistDirectory, LockSerializesConcurrentPublishers) {
  const fs::path dir = fresh_dir("lock");
  constexpr int kWriters = 6;
  constexpr int kEach = 3;
  std::vector<std::thread> pool;
  std::atomic<int> failures{0};
  for (int w = 0; w < kWriters; ++w) {
    pool.emplace_back([&, w] {
      for (int k = 0; k < kEach; ++k) {
        const std::string key = "k" + std::to_string(w) + "_" + std::to_string(k);
        auto e = PersistDirectory::publish(
            dir.string(), key, state_like_image(static_cast<std::uint64_t>(w * 10 + k), w));
        if (!e) failures.fetch_add(1);
      }
    });
  }
  for (std::thread &t : pool) t.join();
  EXPECT_EQ(failures.load(), 0);
  auto d = PersistDirectory::open(dir.string());
  ASSERT_TRUE(d.has_value());
  ASSERT_EQ(d->entries().size(), static_cast<std::size_t>(kWriters * kEach))
      << "a lost manifest update means two publishers overlapped";
  std::vector<std::uint64_t> seqs;
  for (const PersistManifestEntry &e : d->entries()) seqs.push_back(e.seq);
  std::sort(seqs.begin(), seqs.end());
  for (std::size_t i = 0; i < seqs.size(); ++i) EXPECT_EQ(seqs[i], i + 1);
  EXPECT_FALSE(fs::exists(dir / std::string(kPersistLockFileName))) << "the lock is released";
}
