// atx-vol-state-store — the SpiderRock-derived MarketState store CLI (wave-3 lane P1;
// docs/persist.md, docs/storage/PERSIST_FORMAT.md).
//
//   build   --store <root> --date D --out <root> [--from HHMM --to HHMM | --buckets a,b]
//           [--top N] [--parallel N] [--no-extras] [--force] [--created-unix-ns N]
//           [--sha256] [--json <file>]
//   inspect <file.atxms>
//   verify  <file.atxms> | --out <root> --date D
//   bench   --store <root> --date D --out <root> --bucket HHMM [--top N] [--runs N]
//           [--parquet-runs N] [--no-codecs] [--preset <name>] [--json <file>]
//   synth   --store <dir> [--underliers N]   (a SpiderRock-SHAPED synthetic day, for a
//           data-free round trip: synth -> build -> inspect -> verify -> bench)
//
// The store is a DERIVED cache: --out must not lie under --store (the library refuses
// it). The licensed store is read-only; the sanctioned output root is
// C:\atx-cache\persist\ (the default --out).

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/sha256.hpp"
#include "atx/vol/api/core/version.hpp"
#include "atx/vol/api/marketdata/spiderrock_store.hpp"
#include "atx/vol/api/persist/persist_file.hpp"
#include "atx/vol/api/persist/state_archive.hpp"
#include "persist/persist_internal.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace atx::vol; // NOLINT(google-build-using-namespace): tool TU
using Clock = std::chrono::steady_clock;

// The loader-conventions version the archived values follow (orchestrator ruling Q2).
// TODO(integration): replace with kSpiderRockConventionsVersion
constexpr std::uint32_t kStateStoreConventionsVersion = 1;

constexpr int kExitOk = 0;
constexpr int kExitFail = 1;
constexpr int kExitUsage = 2;

struct Args {
  std::string cmd;
  std::string store;
  std::string date{"2026-10-14"};
  std::string out{"C:/atx-cache/persist"};
  std::string from{"1000"};
  std::string to{"1500"};
  std::string bucket{"1000"};
  std::string file;
  std::string json;
  std::string preset{"unknown"};
  std::vector<std::string> buckets;
  std::size_t top{100};
  std::size_t underliers{20};
  unsigned parallel{2};
  bool extras{true};
  bool force{false};
  bool sha256{false};
  bool codecs{true};
  std::int64_t created{0};
  int runs{31};
  int parquet_runs{5};
};

void usage() {
  std::fputs(
      "usage: atx-vol-state-store <build|inspect|verify|bench> [options]\n"
      "  build   --store <root> --date D --out <root> [--from HHMM --to HHMM | --buckets a,b]\n"
      "          [--top N (0 = uncapped)] [--parallel N] [--no-extras] [--force]\n"
      "          [--created-unix-ns N] [--sha256] [--json <file>]\n"
      "  inspect <file.atxms>\n"
      "  verify  <file.atxms> | --out <root> --date D\n"
      "  bench   --store <root> --date D --out <root> --bucket HHMM [--top N] [--runs N]\n"
      "          [--parquet-runs N] [--no-codecs] [--preset <name>] [--json <file>]\n"
      "  synth   --store <dir> [--underliers N]  (writes a synthetic SpiderRock-shaped day)\n",
      stderr);
}

[[nodiscard]] std::vector<std::string> split_csv(std::string_view s) {
  std::vector<std::string> out;
  std::size_t at = 0;
  while (at <= s.size()) {
    const std::size_t comma = s.find(',', at);
    const std::size_t end = comma == std::string_view::npos ? s.size() : comma;
    if (end > at) out.emplace_back(s.substr(at, end - at));
    if (comma == std::string_view::npos) break;
    at = comma + 1;
  }
  return out;
}

[[nodiscard]] std::optional<Args> parse(int argc, char **argv) {
  if (argc < 2) return std::nullopt;
  Args a;
  a.cmd = argv[1];
  const std::span<char *> rest{argv + 2, static_cast<std::size_t>(argc - 2)};
  for (std::size_t i = 0; i < rest.size(); ++i) {
    const std::string_view k = rest[i];
    const auto value = [&]() -> std::optional<std::string> {
      if (i + 1 >= rest.size()) return std::nullopt;
      return std::string(rest[++i]);
    };
    std::optional<std::string> v;
    if (k == "--no-extras") {
      a.extras = false;
    } else if (k == "--force") {
      a.force = true;
    } else if (k == "--sha256") {
      a.sha256 = true;
    } else if (k == "--no-codecs") {
      a.codecs = false;
    } else if (k.rfind("--", 0) != 0) {
      a.file = std::string(k);
    } else if (!(v = value())) {
      return std::nullopt;
    } else if (k == "--store") {
      a.store = *v;
    } else if (k == "--date") {
      a.date = *v;
    } else if (k == "--out") {
      a.out = *v;
    } else if (k == "--from") {
      a.from = *v;
    } else if (k == "--to") {
      a.to = *v;
    } else if (k == "--bucket") {
      a.bucket = *v;
    } else if (k == "--buckets") {
      a.buckets = split_csv(*v);
    } else if (k == "--json") {
      a.json = *v;
    } else if (k == "--preset") {
      a.preset = *v;
    } else if (k == "--top") {
      a.top = static_cast<std::size_t>(std::strtoull(v->c_str(), nullptr, 10));
    } else if (k == "--underliers") {
      a.underliers = static_cast<std::size_t>(std::strtoull(v->c_str(), nullptr, 10));
    } else if (k == "--parallel") {
      a.parallel = static_cast<unsigned>(std::strtoul(v->c_str(), nullptr, 10));
    } else if (k == "--created-unix-ns") {
      a.created = std::strtoll(v->c_str(), nullptr, 10);
    } else if (k == "--runs") {
      a.runs = std::max(1, std::atoi(v->c_str()));
    } else if (k == "--parquet-runs") {
      a.parquet_runs = std::max(1, std::atoi(v->c_str()));
    } else {
      return std::nullopt;
    }
  }
  return a;
}

[[nodiscard]] SpiderRockStoreSpec spec_of(const Args &a) {
  SpiderRockStoreSpec s;
  s.store_root = a.store;
  s.date = a.date;
  s.max_underliers = a.top;
  return s;
}

[[nodiscard]] double since(Clock::time_point t0) {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

struct Stats {
  double min{0}, p50{0}, p95{0};
};
[[nodiscard]] Stats stats_of(std::vector<double> v) {
  if (v.empty()) return {};
  std::sort(v.begin(), v.end());
  const auto rank = [&](double q) {
    const auto r = static_cast<std::size_t>(std::ceil(q * static_cast<double>(v.size())));
    return v[std::min(v.size(), std::max<std::size_t>(r, 1)) - 1];
  };
  return {v.front(), rank(0.50), rank(0.95)};
}

// Busy fraction of all logical CPUs over ~0.5 s and available physical memory.
struct HostLoad {
  double busy{-1.0};
  double avail_gb{-1.0};
  unsigned cpus{0};
};
[[nodiscard]] HostLoad sample_host() {
  HostLoad h;
#if defined(_WIN32)
  const auto ticks = [](const FILETIME &f) {
    return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
  };
  FILETIME i0{}, k0{}, u0{}, i1{}, k1{}, u1{};
  if (::GetSystemTimes(&i0, &k0, &u0) != 0) {
    ::Sleep(500);
    if (::GetSystemTimes(&i1, &k1, &u1) != 0) {
      const double idle = static_cast<double>(ticks(i1) - ticks(i0));
      const double total = static_cast<double>((ticks(k1) - ticks(k0)) + (ticks(u1) - ticks(u0)));
      h.busy = total > 0 ? 1.0 - idle / total : -1.0;
    }
  }
  MEMORYSTATUSEX m{};
  m.dwLength = sizeof m;
  if (::GlobalMemoryStatusEx(&m) != 0) h.avail_gb = static_cast<double>(m.ullAvailPhys) / 1e9;
  SYSTEM_INFO si{};
  ::GetSystemInfo(&si);
  h.cpus = si.dwNumberOfProcessors;
#endif
  return h;
}

// Sequential unbuffered read of the whole file (FILE_FLAG_NO_BUFFERING: a cold-media
// proxy; legal because ATXVPF files are 4096-byte multiples). GB/s, or -1.
[[nodiscard]] double unbuffered_read_gbps(const std::string &path, std::uint64_t size) {
#if defined(_WIN32)
  HANDLE h = ::CreateFileW(fs::path{path}.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) return -1.0;
  constexpr DWORD kChunk = 8u << 20;
  void *buf = ::VirtualAlloc(nullptr, kChunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  double gbps = -1.0;
  if (buf != nullptr) {
    const auto t0 = Clock::now();
    std::uint64_t done = 0;
    DWORD got = 1;
    while (done < size && got != 0) {
      if (::ReadFile(h, buf, kChunk, &got, nullptr) == 0) break;
      done += got;
    }
    const double s = since(t0);
    if (done == size && s > 0) gbps = static_cast<double>(size) / s / 1e9;
    ::VirtualFree(buf, 0, MEM_RELEASE);
  }
  ::CloseHandle(h);
  return gbps;
#else
  static_cast<void>(path);
  static_cast<void>(size);
  return -1.0;
#endif
}

[[nodiscard]] std::string fmt(const char *f, double v) {
  std::array<char, 64> b{};
  std::snprintf(b.data(), b.size(), f, v);
  return b.data();
}

[[nodiscard]] std::string hex64(std::uint64_t v) {
  std::array<char, 24> b{};
  std::snprintf(b.data(), b.size(), "%016llx", static_cast<unsigned long long>(v));
  return b.data();
}

void write_text(const std::string &path, const std::string &text) {
  if (path.empty()) return;
  std::ofstream os(path, std::ios::binary | std::ios::trunc);
  os << text;
}

// ── build ───────────────────────────────────────────────────────────────────

[[nodiscard]] int cmd_build(const Args &a) {
  if (a.store.empty()) {
    usage();
    return kExitUsage;
  }
  std::vector<std::string> buckets = a.buckets;
  if (buckets.empty()) {
    auto all = list_spiderrock_buckets(a.store, a.date);
    if (!all) {
      std::fprintf(stderr, "build: %s\n", all.error().to_string().c_str());
      return kExitFail;
    }
    for (const std::string &b : *all) {
      if (b >= a.from && b <= a.to) buckets.push_back(b);
    }
  }
  StateStoreBuildOptions o;
  o.out_root = a.out;
  o.max_parallel_buckets = std::max(1u, a.parallel);
  o.include_extras = a.extras;
  o.created_unix_ns = a.created;
  o.force = a.force;
  o.conventions_version = kStateStoreConventionsVersion;
  const auto t0 = Clock::now();
  auto r = build_state_store_from_spiderrock(spec_of(a), buckets, o);
  const double total = since(t0);
  if (!r) {
    std::fprintf(stderr, "build: %s\n", to_status(r.error()).error().to_string().c_str());
    return kExitFail;
  }
  std::ostringstream js;
  js << "{\"schema_version\":1,\"tool\":\"atx-vol-state-store build\",\"date\":\"" << a.date
     << "\",\"top\":" << a.top << ",\"extras\":" << (a.extras ? "true" : "false")
     << ",\"conventions_version\":" << kStateStoreConventionsVersion
     << ",\"total_s\":" << fmt("%.4f", total) << ",\"buckets\":[";
  std::printf("%-6s %10s %12s %8s %8s %8s %4s  %s\n", "bucket", "marks", "bytes", "load_s",
              "encode_s", "publ_s", "skip", a.sha256 ? "sha256" : "path");
  for (std::size_t i = 0; i < r->size(); ++i) {
    const StateStoreBucketReport &b = (*r)[i];
    std::string sha;
    if (a.sha256) {
      auto h = atx::core::sha256_file(b.path);
      sha = h ? *h : std::string("?");
    }
    std::printf("%-6s %10zu %12zu %8.3f %8.3f %8.3f %4s  %s\n", b.bucket_et.c_str(), b.marks, b.bytes,
                b.load_s, b.encode_s, b.publish_s, b.skipped ? "yes" : "no",
                a.sha256 ? sha.c_str() : b.path.c_str());
    js << (i ? "," : "") << "{\"bucket\":\"" << b.bucket_et << "\",\"marks\":" << b.marks
       << ",\"bytes\":" << b.bytes << ",\"skipped\":" << (b.skipped ? "true" : "false")
       << ",\"load_s\":" << fmt("%.4f", b.load_s) << ",\"encode_s\":" << fmt("%.4f", b.encode_s)
       << ",\"publish_s\":" << fmt("%.4f", b.publish_s) << ",\"source_fingerprint\":\""
       << hex64(b.source_fingerprint) << "\""
       << ",\"sha256\":\"" << sha << "\"}";
  }
  js << "]}\n";
  std::printf("total %.3f s\n", total);
  write_text(a.json, js.str());
  return kExitOk;
}

// ── inspect / verify ────────────────────────────────────────────────────────

[[nodiscard]] const char *dtype_name(std::uint8_t d) {
  constexpr const char *kNames[] = {"bytes", "u8", "u16", "u32", "u64", "i32", "i64", "f32", "f64", "pod"};
  return d < std::size(kNames) ? kNames[d] : "?";
}

[[nodiscard]] int cmd_inspect(const Args &a) {
  if (a.file.empty()) {
    usage();
    return kExitUsage;
  }
  auto archive = MarketStateArchive::open(a.file, PersistOpenOptions{true, PersistVerify::Framing, false, 0});
  if (!archive) {
    std::fprintf(stderr, "inspect: %s\n", to_status(archive.error()).error().to_string().c_str());
    return kExitFail;
  }
  const PersistFile &f = archive->file();
  const PersistHeader &h = f.header();
  std::printf("file            %s\n", a.file.c_str());
  std::printf("kind            %s  schema %016llx  source %016llx\n",
              to_string(static_cast<PersistKind>(h.kind)), static_cast<unsigned long long>(h.schema_fingerprint),
              static_cast<unsigned long long>(h.source_fingerprint));
  std::printf("file_size       %llu  sections %u  writer %.32s  created %lld\n",
              static_cast<unsigned long long>(h.file_size), h.section_count, h.writer_name.data(),
              static_cast<long long>(h.created_unix_ns));
  std::printf("date / bucket   %.*s / %.*s  conventions v%u  extras %s  uid table %u\n",
              static_cast<int>(archive->date().size()), archive->date().data(),
              static_cast<int>(archive->bucket_et().size()), archive->bucket_et().data(),
              archive->conventions_version(), archive->has_extras() ? "yes" : "no", archive->uid_table_size());
  const MarketState &st = archive->state();
  std::printf("state           ts %lld  underliers %zu  marks %zu\n", static_cast<long long>(st.ts_ns()),
              st.n_underliers(), archive->n_marks());
  std::printf("%5s %-6s %8s %12s %12s %5s %5s\n", "type", "dtype", "stride", "count", "length", "codec", "req");
  std::uint64_t core = 0;
  std::uint64_t extras = 0;
  for (const PersistSectionEntry &e : f.sections()) {
    std::printf("%5u %-6s %8u %12llu %12llu %5u %5s\n", e.type, dtype_name(e.dtype), e.element_stride,
                static_cast<unsigned long long>(e.element_count), static_cast<unsigned long long>(e.length),
                e.codec, (e.flags & kPersistSectionRequired) != 0u ? "yes" : "no");
    if (e.type >= 16 && e.type <= 21) core += e.raw_length;
    if (e.type >= 32 && e.type <= 38) extras += e.raw_length;
  }
  const double n = static_cast<double>(std::max<std::size_t>(1, archive->n_marks()));
  std::printf("bytes / mark    core %.2f  extras %.2f  file %.2f\n", static_cast<double>(core) / n,
              static_cast<double>(extras) / n, static_cast<double>(h.file_size) / n);
  return kExitOk;
}

[[nodiscard]] int verify_one(const std::string &path) {
  auto archive = MarketStateArchive::open(path, PersistOpenOptions{true, PersistVerify::Paranoid, false, 0});
  if (!archive) {
    std::printf("FAIL %s: %s\n", path.c_str(), to_status(archive.error()).error().to_string().c_str());
    return kExitFail;
  }
  if (auto ok = archive->file().verify_all(); !ok) {
    std::printf("FAIL %s: %s\n", path.c_str(), to_status(ok.error()).error().to_string().c_str());
    return kExitFail;
  }
  std::printf("OK   %s (%zu marks)\n", path.c_str(), archive->n_marks());
  return kExitOk;
}

[[nodiscard]] int cmd_verify(const Args &a) {
  if (!a.file.empty()) return verify_one(a.file);
  auto dir = PersistDirectory::open((fs::path{a.out} / ("date=" + a.date)).string());
  if (!dir) {
    std::fprintf(stderr, "verify: %s\n", to_status(dir.error()).error().to_string().c_str());
    return kExitFail;
  }
  int rc = kExitOk;
  for (const PersistManifestEntry &e : dir->entries()) rc = std::max(rc, verify_one(dir->path(e)));
  return rc;
}

// ── bench ───────────────────────────────────────────────────────────────────

struct CodecRow {
  int level;
  bool shuffle;
  double ratio;
  double encode_s;
  double decode_gbps;
};

// Re-frame every section of `src` with zstd at `level` (± shuffle): the cold tier.
[[nodiscard]] std::optional<CodecRow> codec_row(const PersistFile &src, int level, bool shuffle) {
  std::vector<PersistSectionInput> in;
  std::uint64_t raw = 0;
  for (const PersistSectionEntry &e : src.sections()) {
    auto bytes = src.section(e.type);
    if (!bytes) return std::nullopt;
    PersistSectionInput s;
    s.type = e.type;
    s.dtype = static_cast<PersistDType>(e.dtype);
    s.stride = e.element_stride;
    s.required = (e.flags & kPersistSectionRequired) != 0u;
    s.codec = PersistCodec::Zstd;
    s.filter = shuffle ? PersistFilter::ByteShuffle : PersistFilter::None;
    s.level = level;
    s.bytes = *bytes;
    raw += bytes->size();
    in.push_back(s);
  }
  PersistFileInfo info;
  info.kind = PersistKind::MarketState;
  info.schema_fingerprint = src.header().schema_fingerprint;
  const auto t0 = Clock::now();
  auto image = build_persist_file(in, info, 8);
  const double enc = since(t0);
  if (!image) return std::nullopt;
  auto words = std::make_shared<std::vector<std::uint64_t>>((image->size() + 7) / 8);
  std::memcpy(words->data(), image->data(), image->size());
  const auto bytes = std::as_bytes(std::span<const std::uint64_t>{*words}).first(image->size());
  const auto t1 = Clock::now();
  auto decoded = PersistFile::open_bytes(bytes, words, PersistKind::MarketState,
                                         src.header().schema_fingerprint, PersistVerify::Framing);
  const double dec = since(t1);
  if (!decoded) return std::nullopt;
  return CodecRow{level, shuffle, static_cast<double>(raw) / static_cast<double>(image->size()), enc,
                  static_cast<double>(raw) / dec / 1e9};
}

[[nodiscard]] int cmd_bench(const Args &a) {
  if (a.store.empty()) {
    usage();
    return kExitUsage;
  }
  const HostLoad host_before = sample_host();
  const SpiderRockStoreSpec spec = spec_of(a);
  // 1. Parquet load + state build (the path the archive replaces).
  std::vector<double> parquet;
  std::size_t marks = 0;
  for (int i = 0; i < a.parquet_runs; ++i) {
    UnderlierTable table;
    const auto t0 = Clock::now();
    auto bucket = load_spiderrock_bucket(spec, a.bucket, table);
    if (!bucket) {
      std::fprintf(stderr, "bench: %s\n", bucket.error().to_string().c_str());
      return kExitFail;
    }
    auto storage = market_state_from_bucket(*bucket, spec.time);
    auto state = storage ? make_market_state(*storage) : Result<MarketState>{tl::unexpected(storage.error())};
    parquet.push_back(since(t0));
    if (!state) return kExitFail;
    marks = 0;
    for (const auto &m : storage->marks) marks += m.sigma.size();
  }
  // 2. The archive (built if absent).
  auto store = StateStore::open(a.out);
  auto archive = store ? store->open_bucket(a.date, a.bucket)
                       : PersistResult<MarketStateArchive>{tl::unexpected(store.error())};
  if (!archive) {
    StateStoreBuildOptions o;
    o.out_root = a.out;
    o.conventions_version = kStateStoreConventionsVersion;
    const std::vector<std::string> one{a.bucket};
    if (auto b = build_state_store_from_spiderrock(spec, one, o); !b) {
      std::fprintf(stderr, "bench: build: %s\n", to_status(b.error()).error().to_string().c_str());
      return kExitFail;
    }
    store = StateStore::open(a.out);
    archive = store->open_bucket(a.date, a.bucket);
    if (!archive) return kExitFail;
  }
  std::string file_path;
  {
    auto dir = PersistDirectory::open((fs::path{a.out} / ("date=" + a.date)).string());
    if (dir && dir->find(a.bucket) != nullptr) file_path = dir->path(*dir->find(a.bucket));
  }
  const std::uint64_t file_size = archive->file().header().file_size;
  // 3. Warm open + state() per verify level (the file is in the page cache from step 2).
  struct Level {
    const char *name;
    PersistOpenOptions o;
  };
  const Level levels[] = {
      {"framing", {true, PersistVerify::Framing, false, 0}},
      {"lazy", {true, PersistVerify::Lazy, false, 0}},
      {"full", {true, PersistVerify::Full, false, 0}},
      {"full+prefetch", {true, PersistVerify::Full, true, 0}},
      {"paranoid", {true, PersistVerify::Paranoid, false, 0}},
      {"full-read-mode", {false, PersistVerify::Full, false, 0}},
  };
  std::ostringstream js;
  const Stats pq = stats_of(parquet);
  std::printf("bucket %s  marks %zu  file %llu bytes  (%s)\n", a.bucket.c_str(), marks,
              static_cast<unsigned long long>(file_size), file_path.c_str());
  std::printf("host  busy %.1f%%  avail %.2f GB  cpus %u\n", host_before.busy * 100.0, host_before.avail_gb,
              host_before.cpus);
  std::printf("parquet load + state build  runs %d  min %.4f  p50 %.4f  p95 %.4f s\n", a.parquet_runs, pq.min,
              pq.p50, pq.p95);
  js << "{\"schema_version\":1,\"tool\":\"atx-vol-state-store bench\",\"preset\":\"" << a.preset
     << "\",\"date\":\"" << a.date << "\",\"bucket\":\"" << a.bucket << "\",\"top\":" << a.top
     << ",\"marks\":" << marks << ",\"file_bytes\":" << file_size << ",\"host\":{\"busy\":"
     << fmt("%.4f", host_before.busy) << ",\"avail_gb\":" << fmt("%.3f", host_before.avail_gb)
     << ",\"cpus\":" << host_before.cpus << "},\"parquet_s\":{\"runs\":" << a.parquet_runs
     << ",\"min\":" << fmt("%.5f", pq.min) << ",\"p50\":" << fmt("%.5f", pq.p50) << ",\"p95\":"
     << fmt("%.5f", pq.p95) << "},\"open_ms\":{";
  double full_p50 = 0.0;
  for (std::size_t li = 0; li < std::size(levels); ++li) {
    std::vector<double> t;
    for (int r = 0; r < a.runs; ++r) {
      const auto t0 = Clock::now();
      auto x = MarketStateArchive::open(file_path, levels[li].o);
      if (!x) return kExitFail;
      volatile std::size_t sink = x->state().n_underliers();
      static_cast<void>(sink);
      t.push_back(since(t0) * 1e3);
    }
    const Stats s = stats_of(t);
    if (std::string_view{levels[li].name} == "full") full_p50 = s.p50;
    std::printf("open + state()  %-15s runs %d  min %.3f  p50 %.3f  p95 %.3f ms\n", levels[li].name, a.runs,
                s.min, s.p50, s.p95);
    js << (li ? "," : "") << "\"" << levels[li].name << "\":{\"min\":" << fmt("%.4f", s.min)
       << ",\"p50\":" << fmt("%.4f", s.p50) << ",\"p95\":" << fmt("%.4f", s.p95) << "}";
  }
  const double speedup = full_p50 > 0 ? pq.p50 * 1e3 / full_p50 : 0.0;
  std::printf("speed-up (parquet p50 / full open p50)  %.1fx\n", speedup);
  // 4. Unbuffered sequential read (cold-media proxy).
  std::vector<double> gbps;
  for (int r = 0; r < 5; ++r) gbps.push_back(unbuffered_read_gbps(file_path, file_size));
  const Stats nb = stats_of(gbps);
  std::printf("NO_BUFFERING sequential read  p50 %.2f GB/s (min %.2f)\n", nb.p50, nb.min);
  // 5. Bytes per mark.
  std::uint64_t core = 0;
  std::uint64_t extras = 0;
  for (const PersistSectionEntry &e : archive->file().sections()) {
    if (e.type >= 16 && e.type <= 21) core += e.raw_length;
    if (e.type >= 32 && e.type <= 38) extras += e.raw_length;
  }
  const double per = static_cast<double>(std::max<std::size_t>(1, archive->n_marks()));
  std::printf("bytes / mark  core %.2f  extras %.2f  file %.2f\n", static_cast<double>(core) / per,
              static_cast<double>(extras) / per, static_cast<double>(file_size) / per);
  js << "},\"speedup_full_vs_parquet\":" << fmt("%.2f", speedup) << ",\"no_buffering_gbps\":"
     << fmt("%.3f", nb.p50) << ",\"bytes_per_mark\":{\"core\":" << fmt("%.3f", static_cast<double>(core) / per)
     << ",\"extras\":" << fmt("%.3f", static_cast<double>(extras) / per) << "},\"codecs\":[";
  // 6. Cold-tier codecs (informational).
  if (a.codecs) {
    auto src = PersistFile::open(file_path, PersistKind::MarketState, market_state_fingerprint(),
                                 PersistOpenOptions{true, PersistVerify::Framing, false, 0});
    bool first = true;
    for (const int level : {1, 3}) {
      for (const bool shuffle : {false, true}) {
        const auto row = src ? codec_row(*src, level, shuffle) : std::nullopt;
        if (!row) continue;
        std::printf("zstd-%d %-10s ratio %.3f  encode %.3f s (8 threads)  decode %.2f GB/s\n", level,
                    shuffle ? "+shuffle" : "", row->ratio, row->encode_s, row->decode_gbps);
        js << (first ? "" : ",") << "{\"level\":" << level << ",\"shuffle\":" << (shuffle ? "true" : "false")
           << ",\"ratio\":" << fmt("%.4f", row->ratio) << ",\"encode_s\":" << fmt("%.4f", row->encode_s)
           << ",\"decode_gbps\":" << fmt("%.3f", row->decode_gbps) << "}";
        first = false;
      }
    }
    std::printf("zstd library %u\n", persist_detail::zstd_version_number());
  }
  const HostLoad host_after = sample_host();
  std::printf("host after  busy %.1f%%  avail %.2f GB\n", host_after.busy * 100.0, host_after.avail_gb);
  js << "],\"host_after\":{\"busy\":" << fmt("%.4f", host_after.busy) << ",\"avail_gb\":"
     << fmt("%.3f", host_after.avail_gb) << "}}\n";
  write_text(a.json, js.str());
  return kExitOk;
}

// ── synth ───────────────────────────────────────────────────────────────────

[[nodiscard]] int cmd_synth(const Args &a) {
  if (a.store.empty()) {
    usage();
    return kExitUsage;
  }
  SyntheticSpiderRockSpec spec;
  spec.n_underliers = std::max<std::size_t>(1, a.underliers);
  SyntheticStoreReport report;
  if (const Status st = write_synthetic_spiderrock_store(spec, a.store, report); !st) {
    std::fprintf(stderr, "synth: %s\n", st.error().to_string().c_str());
    return kExitFail;
  }
  std::printf("synthetic store %s  date %s  %zu buckets  %zu rows  %.2f s\n", a.store.c_str(),
              spec.date.c_str(), spec.buckets_et.size(), report.rows_written, report.seconds);
  return kExitOk;
}

} // namespace

int main(int argc, char **argv) {
  const std::optional<Args> a = parse(argc, argv);
  if (!a) {
    usage();
    return kExitUsage;
  }
  if (a->cmd == "build") return cmd_build(*a);
  if (a->cmd == "inspect") return cmd_inspect(*a);
  if (a->cmd == "verify") return cmd_verify(*a);
  if (a->cmd == "bench") return cmd_bench(*a);
  if (a->cmd == "synth") return cmd_synth(*a);
  usage();
  return kExitUsage;
}
