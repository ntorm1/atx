// SpiderRock-derived MarketState store (design wave3-persist D11 / D15): a derived,
// fingerprinted cache of the Parquet ingest of record. Layout
// <out_root>/date=YYYY-MM-DD/ = one PersistDirectory keyed by bucket HHMM.
//
// UIDS. A build assigns uids exactly as a serial `load_spiderrock_bucket` over the
// buckets IN ORDER with one UnderlierTable would (the risk bench's assignment), while
// loading up to `max_parallel_buckets` buckets at once: each bucket loads into a
// PRIVATE table, then a serial pass interns its tickers into the day table in its
// private-uid order — which is the loader's own intern order (ascending ticker) — and
// remaps its uid column. `market_state_from_bucket` re-sorts by (uid, key, row), and a
// bucket's rows of one uid keep their file order through the remap, so the state is
// bit-identical to the serial load's. Each archive stores the day table as of its
// bucket (section UidTable), so a later build can replay a skipped bucket's
// assignment and rebuild any bucket whose stored uids no longer agree.

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "atx/vol/api/core/version.hpp"
#include "atx/vol/api/persist/state_archive.hpp"

namespace atx::vol {
namespace {

namespace fs = std::filesystem;
using persist_detail::fold_le;
using persist_detail::fold_str;
using Clock = std::chrono::steady_clock;

[[nodiscard]] double seconds_since(Clock::time_point t0) noexcept {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

[[nodiscard]] bool digits(std::string_view s) noexcept {
  return std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

[[nodiscard]] bool date_ok(std::string_view d) noexcept {
  return d.size() == 10 && d[4] == '-' && d[7] == '-' && digits(d.substr(0, 4)) &&
         digits(d.substr(5, 2)) && digits(d.substr(8, 2));
}

[[nodiscard]] bool bucket_ok(std::string_view b) noexcept { return b.size() == 4 && digits(b); }

[[nodiscard]] PersistStatus status_of(const Error &e) noexcept {
  switch (e.code()) {
  case ErrorCode::NotFound: return PersistStatus::NotFound;
  case ErrorCode::InvalidArgument: return PersistStatus::InvalidArgument;
  default: return PersistStatus::IoError;
  }
}

// Absolute, normalized, '/'-separated, no trailing separator; ASCII-lowered on Windows
// (its paths compare case-insensitively).
[[nodiscard]] std::string normalized(const fs::path &p) {
  std::error_code ec;
  fs::path c = fs::weakly_canonical(p, ec);
  if (ec) c = fs::absolute(p, ec).lexically_normal();
  std::string s = c.generic_string();
#if defined(_WIN32)
  std::transform(s.begin(), s.end(), s.begin(),
                 [](char ch) { return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch); });
#endif
  while (s.size() > 1 && s.back() == '/') s.pop_back();
  return s;
}

// True iff `inner` is `outer` or lies beneath it (component-wise, not a string prefix:
// "<root>-derived" is outside "<root>").
[[nodiscard]] bool inside(const fs::path &inner, const fs::path &outer) {
  const std::string i = normalized(inner);
  const std::string o = normalized(outer);
  return i == o || (i.size() > o.size() && i.compare(0, o.size(), o) == 0 && i[o.size()] == '/');
}

[[nodiscard]] fs::path bucket_dir(const SpiderRockStoreSpec &spec, std::string_view bucket) {
  return fs::path{spec.store_root} / ("date=" + spec.date) / ("bucket_et=" + std::string(bucket));
}

[[nodiscard]] fs::path day_dir(const fs::path &root, std::string_view date) {
  return root / ("date=" + std::string(date));
}

// ── Extras: the bucket row each archived mark came from ──────────────────────

// Per uid, the bucket rows' contiguous run [begin, end) (rows are grouped by uid and,
// within a uid, sorted by (expiry, side, strike, file order)).
struct Run {
  std::uint32_t uid;
  std::size_t begin;
  std::size_t end;
};

[[nodiscard]] std::vector<Run> uid_runs(const SpiderRockBucket &b) {
  std::vector<Run> runs;
  for (std::size_t lo = 0; lo < b.uid.size();) {
    std::size_t hi = lo + 1;
    while (hi < b.uid.size() && b.uid[hi] == b.uid[lo]) ++hi;
    runs.push_back({b.uid[lo], lo, hi});
    lo = hi;
  }
  std::sort(runs.begin(), runs.end(), [](const Run &x, const Run &y) { return x.uid < y.uid; });
  return runs;
}

[[nodiscard]] bool key_less(const SpiderRockBucket &b, std::size_t r, std::int64_t e, Side s,
                            double k) noexcept {
  if (b.expiry_ns[r] != e) return b.expiry_ns[r] < e;
  if (b.side[r] != s) return b.side[r] < s;
  return b.strike[r] < k;
}

// The first row of `run` with the mark's key whose sigma has the mark's exact bits
// (the loader keeps the first VALID row of a key, so this finds the row it kept,
// whatever its duplicate / bad-sigma rule).
[[nodiscard]] std::optional<std::size_t> source_row(const SpiderRockBucket &b, const Run &run,
                                                    std::int64_t e, Side s, double k,
                                                    double sigma) noexcept {
  std::size_t lo = run.begin;
  std::size_t hi = run.end;
  while (lo < hi) { // lower_bound, bounded by log2(run) halvings
    const std::size_t mid = lo + (hi - lo) / 2;
    if (key_less(b, mid, e, s, k)) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  for (std::size_t r = lo;
       r < run.end && b.expiry_ns[r] == e && b.side[r] == s && b.strike[r] == k; ++r) {
    if (std::bit_cast<std::uint64_t>(b.sigma[r]) == std::bit_cast<std::uint64_t>(sigma)) return r;
  }
  return std::nullopt;
}

[[nodiscard]] PersistResult<std::vector<MarkExtrasColumns>>
extras_for(const SpiderRockBucket &b, const MarketStateStorage &s) {
  const std::vector<Run> runs = uid_runs(b);
  const std::vector<double> *cols[] = {&b.bid,         &b.ask,          &b.vendor_gamma,
                                       &b.vendor_vega, &b.vendor_theta, &b.vendor_T,
                                       &b.uprc};
  std::vector<MarkExtrasColumns> out(s.marks.size());
  for (std::size_t mi = 0; mi < s.marks.size(); ++mi) {
    const auto &m = s.marks[mi];
    const auto run = std::lower_bound(runs.begin(), runs.end(), m.uid,
                                      [](const Run &r, std::uint32_t u) { return r.uid < u; });
    if (run == runs.end() || run->uid != m.uid) {
      return persist_error(PersistStatus::IndexBounds, 0, m.uid);
    }
    std::vector<double> *dst[] = {&out[mi].bid,          &out[mi].ask,      &out[mi].vendor_gamma,
                                  &out[mi].vendor_vega,  &out[mi].vendor_theta,
                                  &out[mi].vendor_T,     &out[mi].uprc};
    for (std::size_t k = 0; k < std::size(cols); ++k) {
      if (cols[k]->size() == b.size()) dst[k]->reserve(m.sigma.size());
    }
    for (std::size_t j = 0; j < m.sigma.size(); ++j) {
      const auto r = source_row(b, *run, m.expiry_ns[j], m.side[j], m.strike[j], m.sigma[j]);
      if (!r) return persist_error(PersistStatus::IndexBounds, 0, j);
      for (std::size_t k = 0; k < std::size(cols); ++k) {
        if (cols[k]->size() == b.size()) dst[k]->push_back((*cols[k])[*r]);
      }
    }
  }
  return out;
}

// ── One bucket's work ────────────────────────────────────────────────────────

struct Slot {
  std::string bucket;
  std::uint64_t fp{0};
  std::optional<PersistManifestEntry> existing; // the published generation, if any
  bool loaded{false};
  std::optional<SpiderRockBucket> data;
  UnderlierTable table; // private: this bucket's tickers in the loader's intern order
  PersistError error{PersistStatus::Ok, kPersistNone, 0};
  double load_s{0.0};
};

void load_slot(const SpiderRockStoreSpec &spec, Slot &slot) noexcept {
  const auto t0 = Clock::now();
  try {
    auto b = load_spiderrock_bucket(spec, slot.bucket, slot.table);
    if (b) {
      slot.data.emplace(std::move(*b));
    } else {
      slot.error = PersistError{status_of(b.error()), kPersistNone, 0};
    }
  } catch (const std::exception &) {
    // A worker must not terminate the process (bad_alloc on an uncapped bucket): the
    // failure is reported like any other load error.
    slot.error = PersistError{PersistStatus::IoError, kPersistNone, 0};
  }
  slot.load_s = seconds_since(t0);
  slot.loaded = true;
}

// Load `wave` concurrently. Coarse, I/O-bound tasks on a few dedicated threads rather
// than pricing_executor(): the executor inlines fewer than 4 tasks (no concurrency for
// the default 2 buckets) and a pool worker would inline the loader's own fan-outs.
void load_wave(const SpiderRockStoreSpec &spec, std::span<Slot *const> wave) {
  std::vector<std::jthread> workers;
  workers.reserve(wave.size());
  for (Slot *slot : wave) workers.emplace_back([&spec, slot] { load_slot(spec, *slot); });
} // jthreads join here

// Intern every ticker of `archive`'s stored table into `table` iff the table it was
// written with extends `table` exactly; false (and `table` untouched) otherwise.
[[nodiscard]] bool replay_uids(const MarketStateArchive &archive, UnderlierTable &table) {
  const std::uint32_t n = archive.uid_table_size();
  if (n < table.size()) return false;
  for (std::uint32_t uid = 1; uid <= table.size(); ++uid) {
    if (archive.ticker(uid) != table.ticker(uid)) return false;
  }
  UnderlierTable trial = table;
  for (std::uint32_t uid = static_cast<std::uint32_t>(table.size()) + 1; uid <= n; ++uid) {
    if (trial.intern(archive.ticker(uid)) != uid) return false;
  }
  table = std::move(trial);
  return true;
}

[[nodiscard]] std::optional<StateStoreBucketReport>
try_skip(const Slot &slot, const fs::path &day, const StateStoreBuildOptions &opt,
         UnderlierTable &table) {
  if (opt.force || !slot.existing || slot.existing->source_fingerprint != slot.fp) {
    return std::nullopt;
  }
  auto dir = PersistDirectory::open(day.string());
  if (!dir) return std::nullopt;
  // Full at ANY size (review 2): the skip check is off the latency path (~50 ms Full
  // against the ~1.8 s load it saves), and a Framing check would let payload damage in a
  // published bucket survive every rebuild while its fingerprint matches.
  PersistOpenOptions o = kMarketStateOpenDefaults;
  o.expected_size = slot.existing->file_size;
  auto archive = MarketStateArchive::open(dir->path(*slot.existing), o);
  if (!archive || archive->has_extras() != opt.include_extras ||
      archive->file().header().header_crc32c != slot.existing->header_crc32c ||
      !replay_uids(*archive, table)) {
    return std::nullopt;
  }
  StateStoreBucketReport r;
  r.bucket_et = slot.bucket;
  r.marks = archive->n_marks();
  r.bytes = static_cast<std::size_t>(slot.existing->file_size);
  r.skipped = true;
  r.source_fingerprint = slot.fp;
  r.path = dir->path(*slot.existing);
  return r;
}

// Remap the loaded bucket into the day table, build, encode and publish it.
[[nodiscard]] PersistResult<StateStoreBucketReport>
publish_slot(const SpiderRockStoreSpec &spec, Slot &slot, const fs::path &day,
             const StateStoreBuildOptions &opt, UnderlierTable &table) {
  SpiderRockBucket &b = *slot.data;
  std::vector<std::uint32_t> remap(slot.table.size() + 1, 0);
  for (std::uint32_t uid = 1; uid <= slot.table.size(); ++uid) {
    remap[uid] = table.intern(slot.table.ticker(uid));
  }
  for (std::uint32_t &u : b.uid) u = remap[u];
  StateStoreBucketReport r;
  r.bucket_et = slot.bucket;
  r.source_fingerprint = slot.fp;
  // load_s: everything derived from the Parquet (decode, state build, extras rows);
  // encode_s: encode_market_state alone; publish_s: data file + fsync + manifest.
  const auto t_state = Clock::now();
  BucketStateReport rep;
  auto storage = market_state_from_bucket(b, spec.time, &rep);
  if (!storage) return persist_error(status_of(storage.error()));
  std::vector<MarkExtrasColumns> extras;
  if (opt.include_extras) {
    auto x = extras_for(b, *storage);
    if (!x) return tl::unexpected(x.error());
    extras = std::move(*x);
  }
  r.load_s = slot.load_s + seconds_since(t_state);
  const auto t_encode = Clock::now();
  MarketStateWriteInput in;
  in.storage = &*storage;
  in.tickers_by_uid = table.tickers();
  in.extras = extras;
  in.report = &rep;
  in.date = spec.date;
  in.bucket_et = slot.bucket;
  in.conventions_version = opt.conventions_version;
  in.source_fingerprint = slot.fp;
  in.created_unix_ns = opt.created_unix_ns;
  auto image = encode_market_state(in);
  if (!image) return tl::unexpected(image.error());
  r.encode_s = seconds_since(t_encode);
  const auto t_publish = Clock::now();
  auto entry = PersistDirectory::publish(day.string(), slot.bucket, *image,
                                         static_cast<std::uint32_t>(rep.n_marks));
  if (!entry) return tl::unexpected(entry.error());
  r.publish_s = seconds_since(t_publish);
  r.marks = rep.n_marks;
  r.bytes = image->size();
  r.path = (day / std::string(entry->file_name.data())).string();
  slot.data.reset(); // free the bucket before the next wave loads
  return r;
}

[[nodiscard]] PersistOutcome check_build(const SpiderRockStoreSpec &spec,
                                         std::span<const std::string> buckets,
                                         const StateStoreBuildOptions &opt) {
  if (opt.conventions_version == 0 || opt.out_root.empty() || opt.max_parallel_buckets == 0 ||
      !date_ok(spec.date) || spec.store_root.empty()) {
    return persist_error(PersistStatus::InvalidArgument);
  }
  // The store is DERIVED: never inside its source (and never under the licensed root).
  if (inside(fs::path{opt.out_root}, fs::path{spec.store_root})) {
    return persist_error(PersistStatus::InvalidArgument);
  }
  for (std::size_t i = 0; i < buckets.size(); ++i) {
    if (!bucket_ok(buckets[i])) {
      return persist_error(PersistStatus::InvalidArgument, kPersistNone, i);
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (buckets[j] == buckets[i]) {
        return persist_error(PersistStatus::DuplicateKey, kPersistNone, i);
      }
    }
  }
  return {};
}

// The current generation of (date, bucket) under `root`, opened with `options` or, when
// null, with the size policy of its manifest entry (market_state_open_options).
[[nodiscard]] PersistResult<MarketStateArchive> open_entry(std::string_view root,
                                                          std::string_view date,
                                                          std::string_view bucket_et,
                                                          const PersistOpenOptions *options) {
  if (!date_ok(date) || !bucket_ok(bucket_et)) return persist_error(PersistStatus::InvalidArgument);
  auto dir = PersistDirectory::open(day_dir(fs::path{std::string(root)}, date).string());
  if (!dir) return tl::unexpected(dir.error());
  const PersistManifestEntry *e = dir->find(bucket_et);
  if (e == nullptr) return persist_error(PersistStatus::NotFound);
  PersistOpenOptions o =
      options != nullptr ? *options : market_state_open_options(e->file_size);
  o.expected_size = e->file_size;
  auto archive = MarketStateArchive::open(dir->path(*e), o);
  if (!archive) {
    if (archive.error().status == PersistStatus::SizeMismatch) {
      return persist_error(PersistStatus::SegmentMismatch, kPersistNone, e->seq);
    }
    return tl::unexpected(archive.error());
  }
  if (archive->file().header().header_crc32c != e->header_crc32c ||
      archive->file().footer().file_crc32c != e->file_crc32c) {
    return persist_error(PersistStatus::SegmentMismatch, kPersistNone, e->seq);
  }
  return archive;
}

} // namespace

PersistResult<std::uint64_t>
spiderrock_state_source_fingerprint(const SpiderRockStoreSpec &spec, std::string_view bucket_et,
                                    std::uint32_t conventions_version) {
  const fs::path dir = bucket_dir(spec, bucket_et);
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return persist_error(PersistStatus::NotFound);
  std::vector<fs::path> files;
  for (const auto &entry : fs::directory_iterator(dir, ec)) {
    if (entry.is_regular_file(ec) && entry.path().extension() == ".parquet") {
      files.push_back(entry.path());
    }
  }
  if (ec) return persist_error(PersistStatus::IoError);
  if (files.empty()) return persist_error(PersistStatus::NotFound);
  std::sort(files.begin(), files.end());
  std::uint64_t h = persist_detail::kFnvOffset;
  h = fold_le(h, persist_salt("ATXVSS01"), 8);
  h = fold_le(h, conventions_version, 4);
  h = fold_le(h, static_cast<std::uint64_t>(ATX_VOL_VERSION), 8);
  // EVERY SpiderRockStoreSpec field (design D11). The structured bindings are exhaustive:
  // a field added to the spec (or to TimeSpec / VolTimeParams) stops this compiling until
  // it is folded here too, so a build with a different loader option can never skip an
  // archive built under another (review: clock, carry_mode and spot_framework_only were
  // missing).
  const auto &[store_root, date, underliers, max_underliers, time, american_only,
               standard_roots_only, spot_framework_only, carry_mode, clock] = spec;
  const auto &[convention, vol_time] = time;
  const auto &[alpha, trading_hours, nontrading_hours, session_open, session_span] = vol_time;
  h = fold_str(h, fs::path{store_root}.lexically_normal().generic_string());
  h = fold_str(h, date);
  h = fold_le(h, underliers.size(), 4);
  for (const std::string &u : underliers) h = fold_str(h, u);
  h = fold_le(h, max_underliers, 8);
  h = fold_le(h, static_cast<std::uint8_t>(convention), 1);
  for (const double v : {alpha, trading_hours, nontrading_hours, session_open, session_span}) {
    h = fold_le(h, std::bit_cast<std::uint64_t>(v), 8);
  }
  h = fold_le(h, american_only ? 1u : 0u, 1);
  h = fold_le(h, standard_roots_only ? 1u : 0u, 1);
  h = fold_le(h, spot_framework_only ? 1u : 0u, 1);
  h = fold_le(h, static_cast<std::uint8_t>(carry_mode), 1);
  h = fold_le(h, static_cast<std::uint8_t>(clock), 1);
  h = fold_str(h, bucket_et);
  h = fold_le(h, files.size(), 4);
  for (const fs::path &f : files) {
    const std::uintmax_t size = fs::file_size(f, ec);
    if (ec) return persist_error(PersistStatus::IoError);
    const auto mtime = fs::last_write_time(f, ec);
    if (ec) return persist_error(PersistStatus::IoError);
    h = fold_str(h, f.filename().generic_string());
    h = fold_le(h, size, 8);
    h = fold_le(h, static_cast<std::uint64_t>(mtime.time_since_epoch().count()), 8);
  }
  return persist_detail::fmix64(h);
}

PersistResult<std::vector<StateStoreBucketReport>>
build_state_store_from_spiderrock(const SpiderRockStoreSpec &spec,
                                  std::span<const std::string> buckets_et,
                                  const StateStoreBuildOptions &options) {
  if (PersistOutcome ok = check_build(spec, buckets_et, options); !ok) {
    return tl::unexpected(ok.error());
  }
  const fs::path day = day_dir(fs::path{options.out_root}, spec.date);
  auto existing = PersistDirectory::open(day.string());
  if (!existing && existing.error().status != PersistStatus::NotFound) {
    return tl::unexpected(existing.error());
  }
  std::vector<Slot> slots(buckets_et.size());
  for (std::size_t i = 0; i < slots.size(); ++i) {
    slots[i].bucket = buckets_et[i];
    auto fp = spiderrock_state_source_fingerprint(spec, buckets_et[i], options.conventions_version);
    if (!fp) return tl::unexpected(fp.error());
    slots[i].fp = *fp;
    if (existing) {
      if (const PersistManifestEntry *e = existing->find(buckets_et[i]); e != nullptr) {
        slots[i].existing = *e;
      }
    }
  }
  const auto skippable = [&](const Slot &s) {
    return !options.force && s.existing && s.existing->source_fingerprint == s.fp;
  };
  UnderlierTable table; // the day table, grown in bucket order
  std::vector<StateStoreBucketReport> reports;
  reports.reserve(slots.size());
  for (std::size_t k = 0; k < slots.size(); ++k) {
    if (skippable(slots[k])) {
      if (auto r = try_skip(slots[k], day, options, table); r) {
        reports.push_back(std::move(*r));
        continue;
      }
    }
    if (!slots[k].loaded) {
      // A wave: k plus the next buckets that certainly need a load (no up-to-date
      // generation) and are not loaded yet, up to max_parallel_buckets in flight; they are
      // published in order. (A bucket loaded by an earlier wave stays loaded when a
      // skippable bucket before it fails its skip and opens a new wave.)
      std::vector<Slot *> wave{&slots[k]};
      for (std::size_t j = k + 1;
           j < slots.size() && wave.size() < options.max_parallel_buckets; ++j) {
        if (!skippable(slots[j]) && !slots[j].loaded) wave.push_back(&slots[j]);
      }
      load_wave(spec, wave);
    }
    if (slots[k].error.status != PersistStatus::Ok) return tl::unexpected(slots[k].error);
    auto r = publish_slot(spec, slots[k], day, options, table);
    if (!r) return tl::unexpected(r.error());
    reports.push_back(std::move(*r));
  }
  return reports;
}

PersistResult<StateStore> StateStore::open(std::string_view root) {
  std::error_code ec;
  if (!fs::is_directory(fs::path{std::string(root)}, ec)) {
    return persist_error(PersistStatus::NotFound);
  }
  StateStore s;
  s.root_ = std::string(root);
  return s;
}

PersistResult<std::vector<std::string>> StateStore::buckets(std::string_view date) const {
  if (!date_ok(date)) return persist_error(PersistStatus::InvalidArgument);
  auto dir = PersistDirectory::open(day_dir(fs::path{root_}, date).string());
  if (!dir) return tl::unexpected(dir.error());
  std::vector<std::string> out;
  for (const PersistManifestEntry &e : dir->entries()) out.emplace_back(e.key.data());
  return out;
}

PersistResult<MarketStateArchive> StateStore::open_bucket(std::string_view date,
                                                          std::string_view bucket_et) const {
  return open_entry(root_, date, bucket_et, nullptr);
}

PersistResult<MarketStateArchive> StateStore::open_bucket(std::string_view date,
                                                          std::string_view bucket_et,
                                                          const PersistOpenOptions &options) const {
  return open_entry(root_, date, bucket_et, &options);
}

} // namespace atx::vol
