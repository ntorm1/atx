// RiskRun (kind RiskRun): a directory of self-contained `seg-<seq>.atxrr` segments
// behind a PersistDirectory manifest (design wave3-persist D13; layout spec
// docs/storage/PERSIST_HISTORY_FORMAT.md, records in persist/risk_run_format.hpp).
//
// WRITER. RiskRunWriter stages one segment in memory (SoA vectors, reserved once, so a
// steady-state append is a handful of memcpys) and seals it — frames the staged spans
// straight into a PersistFile image and publishes it through PersistDirectory (file
// first, then the manifest) — when a NEW step would start while the staged bytes have
// reached `segment_bytes`, and on finish(). A step therefore never straddles two
// segments: every segment holds whole steps plus the book, and opens on its own.
// A step is opened by append_frame (its instant is the frame's) and an explain whose
// shifted instant is that step's attaches to it; any other explain opens its own step.
//
// READER. RiskRun::Reader (the private, full API; RiskRunArchive is its facade) opens
// every listed segment, checks each against its manifest entry and the book identity
// of segment 0, and indexes the steps globally. Column payloads stay lazy: a column's
// CRC is verified on its first touch (PersistFile Lazy), the index sections at open.
//
// COMPACTION. compact_risk_run reads the run fully verified, concatenates every
// column's bytes in step order (bit-exact: no value is re-encoded), re-bases the step
// and VaR indices, optionally zstd + byte-shuffles every section (the cold tier) and
// replaces the manifest with the one merged segment under the directory lock. The
// superseded segment files stay on disk (a reader may map them; offline gc removes
// them).

#include "atx/vol/api/persist/risk_run_archive.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/api/persist/persist_file.hpp"
#include "persist/persist_internal.hpp"
#include "persist/risk_run_format.hpp"
#include "storage/writer_lock.hpp"

namespace atx::vol {

std::uint64_t risk_run_fingerprint() noexcept { return risk_run::kFingerprint; }

namespace risk_run {
namespace {

static_assert(std::endian::native == std::endian::little, "RiskRun is little-endian only");

namespace fs = std::filesystem;
using S = PersistStatus;

[[nodiscard]] constexpr std::size_t slot(Section s, std::uint32_t plus = 0) noexcept {
  return slot_of(type_of(s) + plus);
}
static_assert(slot(Section::Meta) == 0 && slot(Section::VarPoints) == kSectionCount - 1);

constexpr std::uint8_t kMaxRoute = static_cast<std::uint8_t>(RiskRoute::Refused);
constexpr std::uint8_t kMaxStatus = static_cast<std::uint8_t>(RiskLaneStatus::Refused);

template <class T> [[nodiscard]] std::span<const std::byte> bytes_of(const std::vector<T> &v) noexcept {
  return std::as_bytes(std::span<const T>{v});
}

template <class T> void append(std::vector<T> &dst, std::span<const T> src) {
  dst.insert(dst.end(), src.begin(), src.end());
}

// ── Book ──────────────────────────────────────────────────────────────────────

struct Book {
  std::uint32_t n_option_legs{0};
  std::uint32_t n_share_legs{0};
  std::uint32_t n_unique{0};
  std::uint32_t n_uids{0};
  std::uint64_t logical_id{0};
  std::uint64_t fingerprint{0};
  std::vector<RiskRunLeg> legs;
  std::vector<RiskRunUnique> unique;
  std::vector<std::uint32_t> uids;
  std::vector<std::uint64_t> leg_ids;
};

[[nodiscard]] std::uint64_t book_fingerprint(std::span<const RiskRunLeg> legs,
                                             std::span<const RiskRunUnique> unique,
                                             std::span<const std::uint32_t> uids,
                                             std::span<const std::uint64_t> ids) noexcept {
  std::uint64_t h = persist_detail::kFnvOffset;
  const auto fold = [&h](std::span<const std::byte> b) {
    h = persist_detail::fold_le(h, b.size(), 8);
    for (const std::byte x : b) {
      h ^= std::to_integer<std::uint64_t>(x);
      h *= persist_detail::kFnvPrime;
    }
  };
  fold(std::as_bytes(legs));
  fold(std::as_bytes(unique));
  fold(std::as_bytes(uids));
  fold(std::as_bytes(ids));
  return persist_detail::fmix64(h);
}

[[nodiscard]] PersistResult<Book> capture_book(const PreparedRiskBook &p) {
  const RiskBook &b = p.book();
  constexpr std::size_t kMax = std::numeric_limits<std::uint32_t>::max();
  if (b.n_option_legs() > kMax || b.n_share_legs() > kMax || p.n_unique() > kMax ||
      p.uids().size() > kMax || p.leg_prepared_row().size() != b.n_option_legs() ||
      p.share_uid_slot().size() != b.n_share_legs() || p.book_logical_id() == 0u) {
    return persist_error(S::InvalidArgument, type_of(Section::BookLegs));
  }
  Book k;
  k.n_option_legs = static_cast<std::uint32_t>(b.n_option_legs());
  k.n_share_legs = static_cast<std::uint32_t>(b.n_share_legs());
  k.n_unique = static_cast<std::uint32_t>(p.n_unique());
  k.n_uids = static_cast<std::uint32_t>(p.uids().size());
  k.logical_id = p.book_logical_id();
  k.legs.reserve(b.n_option_legs() + b.n_share_legs());
  k.leg_ids.reserve(b.n_option_legs() + b.n_share_legs());
  for (std::size_t i = 0; i < b.n_option_legs(); ++i) {
    const OptionLeg &l = b.option_legs()[i];
    k.legs.push_back(RiskRunLeg{l.uid, static_cast<std::uint8_t>(ExplainLegKind::Option),
                                static_cast<std::uint8_t>(l.side), 0, p.leg_prepared_row()[i], 0,
                                l.K, l.expiry_ns, l.qty, l.multiplier});
    k.leg_ids.push_back(l.id);
  }
  for (std::size_t j = 0; j < b.n_share_legs(); ++j) {
    const ShareLeg &s = b.share_legs()[j];
    k.legs.push_back(RiskRunLeg{s.uid, static_cast<std::uint8_t>(ExplainLegKind::Share), 0, 0,
                                p.share_uid_slot()[j], 0, 0.0, 0, s.qty, 1.0});
    k.leg_ids.push_back(s.id);
  }
  k.unique.reserve(p.n_unique());
  for (std::size_t r = 0; r < p.n_unique(); ++r) {
    k.unique.push_back(RiskRunUnique{p.uid()[r], static_cast<std::uint8_t>(p.side()[r]), 0, 0,
                                     p.strike()[r], p.expiry_ns()[r]});
  }
  k.uids.assign(p.uids().begin(), p.uids().end());
  k.fingerprint = book_fingerprint(k.legs, k.unique, k.uids, k.leg_ids);
  return k;
}

// ── Segment image ─────────────────────────────────────────────────────────────

struct SegmentParts {
  RiskRunMeta meta{};
  std::array<std::span<const std::byte>, kSectionCount> bytes{}; // kSchema order
  std::int64_t ts_min_ns{0};
  std::int64_t ts_max_ns{0};
};

[[nodiscard]] PersistResult<std::vector<std::byte>> build_segment(const SegmentParts &p,
                                                                  PersistCodec codec, int level,
                                                                  unsigned n_threads) {
  std::array<PersistSectionInput, kSectionCount> in{};
  const PersistFilter filter = codec == PersistCodec::None ? PersistFilter::None
                                                           : PersistFilter::ByteShuffle;
  for (std::size_t i = 0; i < kSectionCount; ++i) {
    const PersistSectionSpec &s = kSchema[i];
    in[i] = PersistSectionInput{s.type, s.type_major, 0, s.dtype, s.stride, true, codec, filter,
                                level, p.bytes[i]};
  }
  in[slot(Section::Meta)].bytes = std::as_bytes(std::span{&p.meta, 1});
  PersistFileInfo info;
  info.kind = PersistKind::RiskRun;
  info.schema_fingerprint = kFingerprint;
  info.created_unix_ns = p.meta.created_unix_ns;
  info.ts_min_ns = p.ts_min_ns;
  info.ts_max_ns = p.ts_max_ns;
  info.source_fingerprint = p.meta.book_fingerprint;
  return build_persist_file(in, info, n_threads);
}

void set_book_parts(SegmentParts &p, std::span<const std::byte> legs, std::span<const std::byte> unique,
                    std::span<const std::byte> uids, std::span<const std::byte> ids) noexcept {
  p.bytes[slot(Section::BookLegs)] = legs;
  p.bytes[slot(Section::BookUnique)] = unique;
  p.bytes[slot(Section::BookUids)] = uids;
  p.bytes[slot(Section::LegIds)] = ids;
}

// ── Writer core ───────────────────────────────────────────────────────────────

struct Staging {
  std::vector<RiskRunStep> steps;
  std::vector<ExplainTotals> totals;
  std::array<std::vector<double>, kRiskFrameColumnCount> frame_f64;
  std::vector<RiskRoute> frame_route;
  std::vector<RiskFlags> frame_flags;
  std::vector<RiskLaneStatus> frame_status;
  std::vector<double> uid_spot;
  std::vector<double> uid_rate;
  std::vector<RiskLaneStatus> uid_status;
  std::vector<double> share_mark;
  std::vector<RiskLaneStatus> share_status;
  std::array<std::vector<double>, kRiskExplainColumnCount> explain_f64;
  std::vector<RiskRoute> route_base;
  std::vector<RiskRoute> route_shifted;
  std::vector<RiskFlags> explain_flags;
  std::vector<RiskLaneStatus> explain_status;
  std::vector<RiskRunBucket> by_uid;
  std::vector<RiskRunBucket> by_expiry;
  std::vector<RiskRunVar> vars;
  std::vector<RiskRunVarFrame> var_frames;
  std::vector<RiskRunVarPoint> var_points;
  std::uint64_t n_frames{0};
  std::uint64_t bytes{0}; // staged payload bytes (the seal threshold's measure)

  [[nodiscard]] bool empty() const noexcept { return steps.empty() && vars.empty(); }
  // Keeps every capacity: a steady-state segment allocates nothing new.
  void clear() noexcept {
    steps.clear();
    totals.clear();
    for (auto &c : frame_f64) c.clear();
    frame_route.clear();
    frame_flags.clear();
    frame_status.clear();
    uid_spot.clear();
    uid_rate.clear();
    uid_status.clear();
    share_mark.clear();
    share_status.clear();
    for (auto &c : explain_f64) c.clear();
    route_base.clear();
    route_shifted.clear();
    explain_flags.clear();
    explain_status.clear();
    by_uid.clear();
    by_expiry.clear();
    vars.clear();
    var_frames.clear();
    var_points.clear();
    n_frames = 0;
    bytes = 0;
  }
};

[[nodiscard]] std::uint32_t needs_flags(const RiskNeeds &n) noexcept {
  return (n.second_order ? kStepNeedsSecondOrder : 0u) | (n.rho ? kStepNeedsRho : 0u) |
         (n.charm ? kStepNeedsCharm : 0u);
}

struct WriterCore {
  std::string dir;
  RiskRunOptions options{};
  Book book;
  Staging staging;
  std::uint64_t next_step{0};
  std::uint64_t next_var{0};
  std::uint32_t next_segment{0};
  bool finished{false};
  bool reserved{false};

  [[nodiscard]] std::size_t n_legs() const noexcept { return book.legs.size(); }

  [[nodiscard]] std::uint64_t frame_bytes() const noexcept {
    return std::uint64_t{book.n_unique} * (kRiskFrameColumnCount * 8u + 1u + 4u + 1u) +
           std::uint64_t{book.n_uids} * 17u + std::uint64_t{book.n_share_legs} * 9u;
  }
  [[nodiscard]] std::uint64_t explain_bytes(std::size_t rows) const noexcept {
    return std::uint64_t{rows} * (kRiskExplainColumnCount * 8u + 1u + 1u + 4u + 1u);
  }

  // Size a staging column for `n` elements and touch every page once (resize writes
  // zeros, clear keeps the capacity): steady-state appends are then pure memcpy, with
  // neither a reallocation nor a first-touch page fault inside the append path.
  template <class T> static void prefault(std::vector<T> &v, std::size_t n) {
    if (v.capacity() >= n) return;
    v.resize(n);
    v.clear();
  }

  // One reservation sized to the segment threshold (capped at 64 steps), paid by the
  // first append, so the first segment grows nothing and later segments reuse the
  // capacity (Staging::clear keeps it).
  void reserve_once() {
    if (reserved) return;
    reserved = true;
    const std::uint64_t per_step = (options.frames ? frame_bytes() : 0u) + explain_bytes(n_legs()) +
                                   sizeof(RiskRunStep) + sizeof(ExplainTotals);
    const std::uint64_t want = options.segment_bytes / std::max<std::uint64_t>(per_step, 1u) + 1u;
    const auto steps = static_cast<std::size_t>(std::clamp<std::uint64_t>(want, 1u, 64u));
    prefault(staging.steps, steps);
    prefault(staging.totals, steps);
    if (options.frames) {
      for (auto &c : staging.frame_f64) prefault(c, steps * book.n_unique);
      prefault(staging.frame_route, steps * book.n_unique);
      prefault(staging.frame_flags, steps * book.n_unique);
      prefault(staging.frame_status, steps * book.n_unique);
      prefault(staging.uid_spot, steps * book.n_uids);
      prefault(staging.uid_rate, steps * book.n_uids);
      prefault(staging.uid_status, steps * book.n_uids);
      prefault(staging.share_mark, steps * book.n_share_legs);
      prefault(staging.share_status, steps * book.n_share_legs);
    }
    for (auto &c : staging.explain_f64) prefault(c, steps * n_legs());
    prefault(staging.route_base, steps * n_legs());
    prefault(staging.route_shifted, steps * n_legs());
    prefault(staging.explain_flags, steps * n_legs());
    prefault(staging.explain_status, steps * n_legs());
  }

  [[nodiscard]] PersistOutcome seal() {
    SegmentParts p;
    RiskRunMeta &m = p.meta;
    m.book_fingerprint = book.fingerprint;
    m.first_step = next_step - staging.steps.size();
    m.n_steps = staging.steps.size();
    m.first_var = next_var - staging.vars.size();
    m.n_var = staging.vars.size();
    m.n_option_legs = book.n_option_legs;
    m.n_share_legs = book.n_share_legs;
    m.n_unique = book.n_unique;
    m.n_uids = book.n_uids;
    m.flags = (options.frames ? kRunFrames : 0u) | (options.buckets ? kRunBuckets : 0u);
    m.segment_ordinal = next_segment;
    m.created_unix_ns = options.created_unix_ns;
    m.n_frames = staging.n_frames;
    m.n_explain_rows = staging.explain_f64[0].size();
    if (!staging.steps.empty()) {
      p.ts_min_ns = std::numeric_limits<std::int64_t>::max();
      p.ts_max_ns = std::numeric_limits<std::int64_t>::min();
      for (const RiskRunStep &s : staging.steps) {
        const bool explained = (s.flags & kStepHasExplain) != 0u;
        p.ts_min_ns = std::min(p.ts_min_ns, explained ? std::min(s.ts_ns, s.base_ts_ns) : s.ts_ns);
        p.ts_max_ns = std::max(p.ts_max_ns, s.ts_ns);
      }
    }
    set_book_parts(p, bytes_of(book.legs), bytes_of(book.unique), bytes_of(book.uids),
                   bytes_of(book.leg_ids));
    const Staging &g = staging;
    p.bytes[slot(Section::Steps)] = bytes_of(g.steps);
    p.bytes[slot(Section::StepTotals)] = bytes_of(g.totals);
    for (std::uint32_t c = 0; c < kRiskFrameColumnCount; ++c) {
      p.bytes[slot(Section::FrameF64, c)] = bytes_of(g.frame_f64[c]);
    }
    p.bytes[slot(Section::FrameRoute)] = bytes_of(g.frame_route);
    p.bytes[slot(Section::FrameFlags)] = bytes_of(g.frame_flags);
    p.bytes[slot(Section::FrameStatus)] = bytes_of(g.frame_status);
    p.bytes[slot(Section::FrameUidSpot)] = bytes_of(g.uid_spot);
    p.bytes[slot(Section::FrameUidRate)] = bytes_of(g.uid_rate);
    p.bytes[slot(Section::FrameUidStatus)] = bytes_of(g.uid_status);
    p.bytes[slot(Section::FrameShareMark)] = bytes_of(g.share_mark);
    p.bytes[slot(Section::FrameShareStatus)] = bytes_of(g.share_status);
    for (std::uint32_t c = 0; c < kRiskExplainColumnCount; ++c) {
      p.bytes[slot(Section::ExplainF64, c)] = bytes_of(g.explain_f64[c]);
    }
    p.bytes[slot(Section::ExplainRouteBase)] = bytes_of(g.route_base);
    p.bytes[slot(Section::ExplainRouteShifted)] = bytes_of(g.route_shifted);
    p.bytes[slot(Section::ExplainFlags)] = bytes_of(g.explain_flags);
    p.bytes[slot(Section::ExplainStatus)] = bytes_of(g.explain_status);
    p.bytes[slot(Section::BucketsByUid)] = bytes_of(g.by_uid);
    p.bytes[slot(Section::BucketsByExpiry)] = bytes_of(g.by_expiry);
    p.bytes[slot(Section::VarIndex)] = bytes_of(g.vars);
    p.bytes[slot(Section::VarFrames)] = bytes_of(g.var_frames);
    p.bytes[slot(Section::VarPoints)] = bytes_of(g.var_points);
    auto image = build_segment(p, PersistCodec::None, 3, 1);
    if (!image) return tl::unexpected(image.error());
    const auto records = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(m.n_steps, std::numeric_limits<std::uint32_t>::max()));
    auto published = PersistDirectory::publish(dir, segment_key(next_segment), *image, records);
    if (!published) return tl::unexpected(published.error());
    staging.clear();
    ++next_segment;
    return {};
  }

  // A new step is about to start: seal first when the staged segment is full.
  [[nodiscard]] PersistOutcome before_new_step() {
    if (!staging.steps.empty() && staging.bytes >= options.segment_bytes) return seal();
    return {};
  }

  [[nodiscard]] bool frame_shape_ok(const RiskFrame &f) const noexcept {
    const auto n = static_cast<std::size_t>(book.n_unique);
    for (const auto col : kFrameMembers) {
      if ((f.*col).size() != n) return false;
    }
    return f.route.size() == n && f.flags.size() == n && f.status.size() == n &&
           f.uid_spot.size() == book.n_uids && f.uid_flat_rate.size() == book.n_uids &&
           f.uid_status.size() == book.n_uids && f.share_mark.size() == book.n_share_legs &&
           f.share_status.size() == book.n_share_legs;
  }

  [[nodiscard]] PersistOutcome append_frame(const RiskFrame &f) {
    if (finished) return persist_error(S::InvalidArgument);
    if (f.book_logical_id != book.logical_id) {
      return persist_error(S::InvalidArgument, type_of(Section::Steps), f.book_logical_id);
    }
    if (!frame_shape_ok(f)) return persist_error(S::InvalidArgument, type_of(Section::FrameF64));
    if (PersistOutcome ok = before_new_step(); !ok) return ok;
    reserve_once();
    RiskRunStep s{};
    s.step = next_step;
    s.ts_ns = f.ts_ns;
    s.frame_index = options.frames ? staging.n_frames : kNone64;
    s.explain_row = kNone64;
    s.bucket_uid_begin = staging.by_uid.size();
    s.bucket_expiry_begin = staging.by_expiry.size();
    s.book_logical_id = f.book_logical_id;
    s.flags = needs_flags(f.needs) | (options.frames ? kStepHasFrame : 0u);
    s.counters = f.counters;
    staging.steps.push_back(s);
    staging.totals.push_back(ExplainTotals{});
    staging.bytes += sizeof(RiskRunStep) + sizeof(ExplainTotals);
    if (options.frames) {
      for (std::size_t c = 0; c < kRiskFrameColumnCount; ++c) {
        append(staging.frame_f64[c], std::span<const double>{f.*kFrameMembers[c]});
      }
      append(staging.frame_route, std::span<const RiskRoute>{f.route});
      append(staging.frame_flags, std::span<const RiskFlags>{f.flags});
      append(staging.frame_status, std::span<const RiskLaneStatus>{f.status});
      append(staging.uid_spot, std::span<const double>{f.uid_spot});
      append(staging.uid_rate, std::span<const double>{f.uid_flat_rate});
      append(staging.uid_status, std::span<const RiskLaneStatus>{f.uid_status});
      append(staging.share_mark, std::span<const double>{f.share_mark});
      append(staging.share_status, std::span<const RiskLaneStatus>{f.share_status});
      ++staging.n_frames;
      staging.bytes += frame_bytes();
    }
    ++next_step;
    return {};
  }

  // The rows must be the book's legs in explain order (every option leg, then — when
  // shares are included — every share leg).
  [[nodiscard]] PersistOutcome check_explain(const ExplainResult &r) const noexcept {
    const std::size_t rows = r.size();
    if (rows != n_legs() && rows != book.n_option_legs) {
      return persist_error(S::InvalidArgument, type_of(Section::LegIds), rows);
    }
    for (const auto col : kExplainMembers) {
      if ((r.*col).size() != rows) return persist_error(S::InvalidArgument, type_of(Section::ExplainF64));
    }
    if (r.uid.size() != rows || r.kind.size() != rows || r.route_base.size() != rows ||
        r.route_shifted.size() != rows || r.flags.size() != rows || r.status.size() != rows ||
        r.by_uid.size() > std::numeric_limits<std::uint32_t>::max() ||
        r.by_expiry.size() > std::numeric_limits<std::uint32_t>::max()) {
      return persist_error(S::InvalidArgument, type_of(Section::ExplainStatus));
    }
    for (std::size_t i = 0; i < rows; ++i) {
      if (r.id[i] != book.leg_ids[i] || r.uid[i] != book.legs[i].uid ||
          static_cast<std::uint8_t>(r.kind[i]) != book.legs[i].kind) {
        return persist_error(S::InvalidArgument, type_of(Section::LegIds), i);
      }
    }
    return {};
  }

  [[nodiscard]] PersistOutcome append_explain(const ExplainResult &r) {
    if (finished) return persist_error(S::InvalidArgument);
    if (PersistOutcome ok = check_explain(r); !ok) return ok;
    const bool attach = !staging.steps.empty() &&
                        (staging.steps.back().flags & kStepHasExplain) == 0u &&
                        staging.steps.back().ts_ns == r.shifted_ts_ns;
    if (!attach) {
      if (PersistOutcome ok = before_new_step(); !ok) return ok;
      reserve_once();
      RiskRunStep s{};
      s.step = next_step++;
      s.ts_ns = r.shifted_ts_ns;
      s.frame_index = kNone64;
      staging.steps.push_back(s);
      staging.totals.push_back(ExplainTotals{});
      staging.bytes += sizeof(RiskRunStep) + sizeof(ExplainTotals);
    }
    RiskRunStep &s = staging.steps.back();
    const std::size_t rows = r.size();
    s.base_ts_ns = r.base_ts_ns;
    s.dt_years = r.dt_years;
    s.explain_row = staging.explain_f64[0].size();
    s.explain_rows = static_cast<std::uint32_t>(rows);
    s.explain_seconds = r.explain_seconds;
    s.flags |= kStepHasExplain | (r.rate_move_without_rho ? kStepRateMoveWithoutRho : 0u);
    s.n_pivot_groups = r.n_pivot_groups;
    s.n_pivot_edge_fallback = r.n_pivot_edge_fallback;
    s.excluded_count = r.excluded_count;
    s.excluded_premium = r.excluded_premium;
    staging.totals.back() = r.total;
    for (std::size_t c = 0; c < kRiskExplainColumnCount; ++c) {
      append(staging.explain_f64[c], std::span<const double>{r.*kExplainMembers[c]});
    }
    append(staging.route_base, std::span<const RiskRoute>{r.route_base});
    append(staging.route_shifted, std::span<const RiskRoute>{r.route_shifted});
    append(staging.explain_flags, std::span<const RiskFlags>{r.flags});
    append(staging.explain_status, std::span<const RiskLaneStatus>{r.status});
    s.bucket_uid_begin = staging.by_uid.size();
    s.bucket_expiry_begin = staging.by_expiry.size();
    if (options.buckets) {
      s.n_bucket_uid = static_cast<std::uint32_t>(r.by_uid.size());
      s.n_bucket_expiry = static_cast<std::uint32_t>(r.by_expiry.size());
      for (const ExplainBucket &b : r.by_uid) {
        staging.by_uid.push_back(RiskRunBucket{s.step, b.key, b.expiry_ns, b.totals});
      }
      for (const ExplainBucket &b : r.by_expiry) {
        staging.by_expiry.push_back(RiskRunBucket{s.step, b.key, b.expiry_ns, b.totals});
      }
      staging.bytes += (r.by_uid.size() + r.by_expiry.size()) * sizeof(RiskRunBucket);
    }
    staging.bytes += explain_bytes(rows);
    return {};
  }

  [[nodiscard]] PersistOutcome append_var(const HistoricalVarResult &r) {
    if (finished) return persist_error(S::InvalidArgument);
    RiskRunVar v{};
    v.step = next_step == 0 ? kNone64 : next_step - 1;
    v.frame_begin = staging.var_frames.size();
    v.frame_count = r.frames.size();
    v.point_begin = staging.var_points.size();
    v.point_count = r.points.size();
    v.n_admitted = r.n_admitted;
    v.reference_pv = r.reference_pv;
    for (const HistoricalVarFrame &f : r.frames) {
      staging.var_frames.push_back(RiskRunVarFrame{f.base_ts_ns, f.shifted_ts_ns, f.totals,
                                                   f.n_legs_resolved, f.n_legs_unresolved,
                                                   static_cast<std::uint8_t>(f.admitted ? 1u : 0u),
                                                   {}});
    }
    for (const HistoricalVarPoint &p : r.points) {
      staging.var_points.push_back(RiskRunVarPoint{p.confidence, p.value_at_risk,
                                                   p.expected_shortfall, p.tail_scenarios,
                                                   p.tail_mean});
    }
    staging.vars.push_back(v);
    staging.bytes += sizeof(RiskRunVar) + r.frames.size() * sizeof(RiskRunVarFrame) +
                     r.points.size() * sizeof(RiskRunVarPoint);
    ++next_var;
    return {};
  }

  [[nodiscard]] PersistOutcome finish() {
    if (finished) return persist_error(S::InvalidArgument);
    // An empty run still publishes one (book-only) segment so it opens.
    if (!staging.empty() || next_segment == 0) {
      if (PersistOutcome ok = seal(); !ok) return ok;
    }
    finished = true;
    return {};
  }
};

// ── Reader core ───────────────────────────────────────────────────────────────

struct SegmentView {
  explicit SegmentView(PersistFile f) noexcept : file{std::move(f)} {}

  PersistFile file;
  RiskRunMeta meta{};
  std::span<const RiskRunStep> steps{};
  std::span<const ExplainTotals> totals{};
  std::span<const RiskRunBucket> by_uid{};
  std::span<const RiskRunBucket> by_expiry{};
  std::span<const RiskRunVar> vars{};
  std::span<const RiskRunVarFrame> var_frames{};
  std::span<const RiskRunVarPoint> var_points{};
  std::span<const RiskRunLeg> legs{};
  std::span<const RiskRunUnique> unique{};
  std::span<const std::uint32_t> uids{};
  std::span<const std::uint64_t> leg_ids{};

  [[nodiscard]] std::uint64_t count(Section s, std::uint32_t plus = 0) const noexcept {
    const PersistSectionEntry *e = file.entry(type_of(s) + plus);
    return e == nullptr ? 0u : e->element_count;
  }
};

template <class T>
[[nodiscard]] PersistOutcome bind_section(const PersistFile &f, Section s, std::span<const T> &out) noexcept {
  auto v = f.typed<T>(type_of(s));
  if (!v) return tl::unexpected(v.error());
  out = *v;
  return {};
}

// Element counts of the column sections against the meta (no payload touched).
[[nodiscard]] PersistOutcome check_columns(const SegmentView &v) noexcept {
  const RiskRunMeta &m = v.meta;
  const bool frames = (m.flags & kRunFrames) != 0u;
  const std::uint64_t fu = frames ? m.n_frames * m.n_unique : 0u;
  const std::uint64_t fi = frames ? m.n_frames * m.n_uids : 0u;
  const std::uint64_t fs_ = frames ? m.n_frames * m.n_share_legs : 0u;
  if (!frames && m.n_frames != 0u) return persist_error(S::RecordInvalid, type_of(Section::Meta));
  for (std::uint32_t c = 0; c < kRiskFrameColumnCount; ++c) {
    if (v.count(Section::FrameF64, c) != fu) return persist_error(S::IndexBounds, type_of(Section::FrameF64) + c);
  }
  const std::pair<Section, std::uint64_t> frame_cols[] = {
      {Section::FrameRoute, fu},   {Section::FrameFlags, fu},     {Section::FrameStatus, fu},
      {Section::FrameUidSpot, fi}, {Section::FrameUidRate, fi},   {Section::FrameUidStatus, fi},
      {Section::FrameShareMark, fs_}, {Section::FrameShareStatus, fs_}};
  for (const auto &[s, n] : frame_cols) {
    if (v.count(s) != n) return persist_error(S::IndexBounds, type_of(s));
  }
  for (std::uint32_t c = 0; c < kRiskExplainColumnCount; ++c) {
    if (v.count(Section::ExplainF64, c) != m.n_explain_rows) {
      return persist_error(S::IndexBounds, type_of(Section::ExplainF64) + c);
    }
  }
  for (const Section s : {Section::ExplainRouteBase, Section::ExplainRouteShifted,
                          Section::ExplainFlags, Section::ExplainStatus}) {
    if (v.count(s) != m.n_explain_rows) return persist_error(S::IndexBounds, type_of(s));
  }
  return {};
}

[[nodiscard]] PersistOutcome check_book(const SegmentView &v) noexcept {
  const RiskRunMeta &m = v.meta;
  const std::uint64_t n_legs = std::uint64_t{m.n_option_legs} + m.n_share_legs;
  if (v.legs.size() != n_legs || v.leg_ids.size() != n_legs || v.unique.size() != m.n_unique ||
      v.uids.size() != m.n_uids) {
    return persist_error(S::IndexBounds, type_of(Section::BookLegs));
  }
  for (std::size_t i = 0; i < v.legs.size(); ++i) {
    const RiskRunLeg &l = v.legs[i];
    const auto kind = static_cast<std::uint8_t>(i < m.n_option_legs ? ExplainLegKind::Option
                                                                    : ExplainLegKind::Share);
    if (l.kind != kind || l.side > 1u || l.reserved0 != 0u || l.reserved1 != 0u) {
      return persist_error(S::RecordInvalid, type_of(Section::BookLegs), i);
    }
  }
  for (std::size_t i = 0; i < v.unique.size(); ++i) {
    const RiskRunUnique &u = v.unique[i];
    if (u.side > 1u || u.reserved0 != 0u || u.reserved1 != 0u) {
      return persist_error(S::RecordInvalid, type_of(Section::BookUnique), i);
    }
  }
  return {};
}

[[nodiscard]] PersistOutcome check_steps(const SegmentView &v) noexcept {
  const RiskRunMeta &m = v.meta;
  const std::uint64_t n_legs = std::uint64_t{m.n_option_legs} + m.n_share_legs;
  std::uint64_t frame = 0;
  std::uint64_t row = 0;
  std::uint64_t buid = 0;
  std::uint64_t bexp = 0;
  for (std::size_t i = 0; i < v.steps.size(); ++i) {
    const RiskRunStep &s = v.steps[i];
    const auto bad = [&] { return persist_error(S::RecordInvalid, type_of(Section::Steps), i); };
    if (s.step != m.first_step + i || (s.flags & ~kStepKnownFlags) != 0u) return bad();
    if ((s.flags & kStepHasFrame) != 0u) {
      if ((m.flags & kRunFrames) == 0u || s.frame_index != frame++) return bad();
    } else if (s.frame_index != kNone64) {
      return bad();
    }
    if ((s.flags & kStepHasExplain) != 0u) {
      if (s.explain_row != row || (s.explain_rows != n_legs && s.explain_rows != m.n_option_legs)) {
        return bad();
      }
      row += s.explain_rows;
    } else if (s.explain_row != kNone64 || s.explain_rows != 0u || s.n_bucket_uid != 0u ||
               s.n_bucket_expiry != 0u) {
      return bad();
    }
    if (s.bucket_uid_begin != buid || s.bucket_expiry_begin != bexp) return bad();
    buid += s.n_bucket_uid;
    bexp += s.n_bucket_expiry;
    if (buid > v.by_uid.size() || bexp > v.by_expiry.size()) {
      return persist_error(S::IndexBounds, type_of(Section::BucketsByUid), i);
    }
    for (const RiskRunBucket &b : v.by_uid.subspan(static_cast<std::size_t>(s.bucket_uid_begin), s.n_bucket_uid)) {
      if (b.step != s.step) return persist_error(S::RecordInvalid, type_of(Section::BucketsByUid), i);
    }
    for (const RiskRunBucket &b :
         v.by_expiry.subspan(static_cast<std::size_t>(s.bucket_expiry_begin), s.n_bucket_expiry)) {
      if (b.step != s.step) return persist_error(S::RecordInvalid, type_of(Section::BucketsByExpiry), i);
    }
  }
  if (frame != m.n_frames || row != m.n_explain_rows || buid != v.by_uid.size() ||
      bexp != v.by_expiry.size()) {
    return persist_error(S::IndexBounds, type_of(Section::Steps));
  }
  std::uint64_t vf = 0;
  std::uint64_t vp = 0;
  for (std::size_t i = 0; i < v.vars.size(); ++i) {
    const RiskRunVar &r = v.vars[i];
    if (r.frame_begin != vf || r.point_begin != vp || r.reserved != 0u) {
      return persist_error(S::RecordInvalid, type_of(Section::VarIndex), i);
    }
    vf += r.frame_count;
    vp += r.point_count;
    if (vf > v.var_frames.size() || vp > v.var_points.size()) {
      return persist_error(S::IndexBounds, type_of(Section::VarIndex), i);
    }
  }
  if (vf != v.var_frames.size() || vp != v.var_points.size()) {
    return persist_error(S::IndexBounds, type_of(Section::VarIndex));
  }
  for (std::size_t i = 0; i < v.var_frames.size(); ++i) {
    const RiskRunVarFrame &f = v.var_frames[i];
    if (f.admitted > 1u || std::any_of(f.reserved.begin(), f.reserved.end(), [](std::uint8_t b) { return b != 0u; })) {
      return persist_error(S::RecordInvalid, type_of(Section::VarFrames), i);
    }
  }
  return {};
}

[[nodiscard]] PersistResult<SegmentView> open_segment(const std::string &path,
                                                      const PersistManifestEntry &e,
                                                      PersistOpenOptions options) {
  options.expected_size = e.file_size;
  auto f = PersistFile::open(path, PersistKind::RiskRun, kFingerprint, options);
  if (!f) return tl::unexpected(f.error());
  if (f->header().header_crc32c != e.header_crc32c || f->footer().file_crc32c != e.file_crc32c) {
    return persist_error(S::SegmentMismatch, kPersistNone, e.seq);
  }
  SegmentView v{std::move(*f)};
  if (PersistOutcome ok = v.file.check_schema(kSchema); !ok) return tl::unexpected(ok.error());
  std::span<const RiskRunMeta> meta;
  if (PersistOutcome ok = bind_section(v.file, Section::Meta, meta); !ok) return tl::unexpected(ok.error());
  if (meta.size() != 1u) return persist_error(S::SectionStride, type_of(Section::Meta), meta.size());
  v.meta = meta.front();
  const RiskRunMeta &m = v.meta;
  if ((m.flags & ~(kRunFrames | kRunBuckets)) != 0u ||
      std::any_of(m.reserved.begin(), m.reserved.end(), [](std::uint64_t x) { return x != 0u; })) {
    return persist_error(S::RecordInvalid, type_of(Section::Meta));
  }
  for (const PersistOutcome ok :
       {bind_section(v.file, Section::Steps, v.steps), bind_section(v.file, Section::StepTotals, v.totals),
        bind_section(v.file, Section::BucketsByUid, v.by_uid), bind_section(v.file, Section::BucketsByExpiry, v.by_expiry),
        bind_section(v.file, Section::VarIndex, v.vars), bind_section(v.file, Section::VarFrames, v.var_frames),
        bind_section(v.file, Section::VarPoints, v.var_points), bind_section(v.file, Section::BookLegs, v.legs),
        bind_section(v.file, Section::BookUnique, v.unique), bind_section(v.file, Section::BookUids, v.uids),
        bind_section(v.file, Section::LegIds, v.leg_ids)}) {
    if (!ok) return tl::unexpected(ok.error());
  }
  if (v.steps.size() != m.n_steps || v.totals.size() != m.n_steps || v.vars.size() != m.n_var) {
    return persist_error(S::IndexBounds, type_of(Section::Steps));
  }
  if (PersistOutcome ok = check_book(v); !ok) return tl::unexpected(ok.error());
  if (PersistOutcome ok = check_columns(v); !ok) return tl::unexpected(ok.error());
  if (PersistOutcome ok = check_steps(v); !ok) return tl::unexpected(ok.error());
  return v;
}

template <class T> [[nodiscard]] bool same_bytes(std::span<const T> a, std::span<const T> b) noexcept {
  return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size_bytes()) == 0);
}

struct ReaderCore {
  std::vector<SegmentView> segs;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> step_ref; // (segment, local)
  std::vector<std::pair<std::uint32_t, std::uint32_t>> var_ref;

  [[nodiscard]] PersistOutcome add(SegmentView v, std::size_t ordinal) {
    const RiskRunMeta &m = v.meta;
    if (m.segment_ordinal != ordinal || m.first_step != step_ref.size() || m.first_var != var_ref.size()) {
      return persist_error(S::SegmentMismatch, type_of(Section::Meta), ordinal);
    }
    if (segs.empty()) {
      if (book_fingerprint(v.legs, v.unique, v.uids, v.leg_ids) != m.book_fingerprint) {
        return persist_error(S::RecordInvalid, type_of(Section::BookLegs));
      }
    } else {
      const SegmentView &first = segs.front();
      const RiskRunMeta &f = first.meta;
      if (m.book_fingerprint != f.book_fingerprint || m.n_option_legs != f.n_option_legs ||
          m.n_share_legs != f.n_share_legs || m.n_unique != f.n_unique || m.n_uids != f.n_uids ||
          m.flags != f.flags || !same_bytes(v.legs, first.legs) || !same_bytes(v.unique, first.unique) ||
          !same_bytes(v.uids, first.uids) || !same_bytes(v.leg_ids, first.leg_ids)) {
        return persist_error(S::SegmentMismatch, type_of(Section::BookLegs), ordinal);
      }
    }
    const auto seg = static_cast<std::uint32_t>(segs.size());
    for (std::size_t i = 0; i < v.steps.size(); ++i) step_ref.emplace_back(seg, static_cast<std::uint32_t>(i));
    for (std::size_t i = 0; i < v.vars.size(); ++i) var_ref.emplace_back(seg, static_cast<std::uint32_t>(i));
    segs.push_back(std::move(v));
    return {};
  }

  struct At {
    const SegmentView *seg{nullptr};
    const RiskRunStep *step{nullptr};
    std::size_t local{0};
  };
  [[nodiscard]] At at(std::size_t step) const noexcept {
    if (step >= step_ref.size()) return {};
    const auto [s, l] = step_ref[step];
    return At{&segs[s], &segs[s].steps[l], l};
  }
};

[[nodiscard]] PersistResult<ReaderCore> open_core(std::string_view dir, const PersistOpenOptions &options) {
  auto d = PersistDirectory::open(dir);
  if (!d) return tl::unexpected(d.error());
  if (d->entries().empty()) return persist_error(S::NotFound);
  ReaderCore core;
  core.segs.reserve(d->entries().size());
  for (std::size_t i = 0; i < d->entries().size(); ++i) {
    const PersistManifestEntry &e = d->entries()[i];
    if (e.kind != static_cast<std::uint32_t>(PersistKind::RiskRun)) {
      return persist_error(S::WrongKind, kPersistNone, e.seq);
    }
    auto v = open_segment(d->path(e), e, options);
    if (!v) return tl::unexpected(v.error());
    if (PersistOutcome ok = core.add(std::move(*v), i); !ok) return tl::unexpected(ok.error());
  }
  return core;
}

template <class E>
[[nodiscard]] PersistOutcome copy_enum(std::span<const std::uint8_t> src, std::uint8_t max, std::uint32_t section,
                                       std::vector<E> &out) {
  out.resize(src.size());
  for (std::size_t i = 0; i < src.size(); ++i) {
    if (src[i] > max) return persist_error(S::RecordInvalid, section, i);
    out[i] = static_cast<E>(src[i]);
  }
  return {};
}

template <class T>
[[nodiscard]] PersistResult<std::span<const T>> rows(const SegmentView &v, std::uint32_t type,
                                                     std::uint64_t begin, std::uint64_t n) noexcept {
  auto col = v.file.typed<T>(type);
  if (!col) return tl::unexpected(col.error());
  if (begin > col->size() || n > col->size() - begin) return persist_error(S::IndexBounds, type, begin);
  return col->subspan(static_cast<std::size_t>(begin), static_cast<std::size_t>(n));
}

[[nodiscard]] PersistResult<RiskFrame> materialize_frame_of(const ReaderCore &c, std::size_t step) {
  const auto a = c.at(step);
  if (a.step == nullptr || (a.step->flags & kStepHasFrame) == 0u) return persist_error(S::NotFound);
  const SegmentView &v = *a.seg;
  const RiskRunStep &s = *a.step;
  const RiskRunMeta &m = v.meta;
  RiskFrame f;
  f.ts_ns = s.ts_ns;
  f.book_logical_id = s.book_logical_id;
  f.needs = RiskNeeds{(s.flags & kStepNeedsSecondOrder) != 0u, (s.flags & kStepNeedsRho) != 0u,
                      (s.flags & kStepNeedsCharm) != 0u};
  f.counters = s.counters;
  const std::uint64_t ru = s.frame_index * m.n_unique;
  for (std::uint32_t col = 0; col < kRiskFrameColumnCount; ++col) {
    auto r = rows<double>(v, type_of(Section::FrameF64) + col, ru, m.n_unique);
    if (!r) return tl::unexpected(r.error());
    (f.*kFrameMembers[col]).assign(r->begin(), r->end());
  }
  auto route = rows<std::uint8_t>(v, type_of(Section::FrameRoute), ru, m.n_unique);
  if (!route) return tl::unexpected(route.error());
  if (PersistOutcome ok = copy_enum(*route, kMaxRoute, type_of(Section::FrameRoute), f.route); !ok) {
    return tl::unexpected(ok.error());
  }
  auto flags = rows<std::uint32_t>(v, type_of(Section::FrameFlags), ru, m.n_unique);
  if (!flags) return tl::unexpected(flags.error());
  f.flags.assign(flags->begin(), flags->end());
  auto status = rows<std::uint8_t>(v, type_of(Section::FrameStatus), ru, m.n_unique);
  if (!status) return tl::unexpected(status.error());
  if (PersistOutcome ok = copy_enum(*status, kMaxStatus, type_of(Section::FrameStatus), f.status); !ok) {
    return tl::unexpected(ok.error());
  }
  // Per-uid and per-share rows of this frame.
  const std::uint64_t ri = s.frame_index * m.n_uids;
  const std::uint64_t rs = s.frame_index * m.n_share_legs;
  const std::pair<Section, std::vector<double> *> f64[] = {
      {Section::FrameUidSpot, &f.uid_spot},
      {Section::FrameUidRate, &f.uid_flat_rate},
      {Section::FrameShareMark, &f.share_mark}};
  for (const auto &[sec, out] : f64) {
    const bool per_uid = sec != Section::FrameShareMark;
    auto x = rows<double>(v, type_of(sec), per_uid ? ri : rs, per_uid ? m.n_uids : m.n_share_legs);
    if (!x) return tl::unexpected(x.error());
    out->assign(x->begin(), x->end());
  }
  const std::pair<Section, std::vector<RiskLaneStatus> *> u8[] = {
      {Section::FrameUidStatus, &f.uid_status}, {Section::FrameShareStatus, &f.share_status}};
  for (const auto &[sec, out] : u8) {
    const bool per_uid = sec == Section::FrameUidStatus;
    auto x = rows<std::uint8_t>(v, type_of(sec), per_uid ? ri : rs, per_uid ? m.n_uids : m.n_share_legs);
    if (!x) return tl::unexpected(x.error());
    if (PersistOutcome ok = copy_enum(*x, kMaxStatus, type_of(sec), *out); !ok) {
      return tl::unexpected(ok.error());
    }
  }
  return f;
}

[[nodiscard]] std::vector<ExplainBucket> buckets_of(std::span<const RiskRunBucket> rows_) {
  std::vector<ExplainBucket> out;
  out.reserve(rows_.size());
  for (const RiskRunBucket &b : rows_) out.push_back(ExplainBucket{b.key, b.expiry_ns, b.totals});
  return out;
}

[[nodiscard]] PersistResult<ExplainResult> materialize_explain_of(const ReaderCore &c, std::size_t step) {
  const auto a = c.at(step);
  if (a.step == nullptr || (a.step->flags & kStepHasExplain) == 0u) return persist_error(S::NotFound);
  const SegmentView &v = *a.seg;
  const RiskRunStep &s = *a.step;
  const std::size_t n = s.explain_rows;
  ExplainResult r;
  r.base_ts_ns = s.base_ts_ns;
  r.shifted_ts_ns = s.ts_ns;
  r.dt_years = s.dt_years;
  r.id.assign(v.leg_ids.begin(), v.leg_ids.begin() + static_cast<std::ptrdiff_t>(n));
  r.uid.reserve(n);
  r.kind.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    r.uid.push_back(v.legs[i].uid);
    r.kind.push_back(static_cast<ExplainLegKind>(v.legs[i].kind)); // validated at open
  }
  for (std::uint32_t col = 0; col < kRiskExplainColumnCount; ++col) {
    auto x = rows<double>(v, type_of(Section::ExplainF64) + col, s.explain_row, n);
    if (!x) return tl::unexpected(x.error());
    (r.*kExplainMembers[col]).assign(x->begin(), x->end());
  }
  const std::pair<Section, std::vector<RiskRoute> *> routes[] = {
      {Section::ExplainRouteBase, &r.route_base}, {Section::ExplainRouteShifted, &r.route_shifted}};
  for (const auto &[sec, out] : routes) {
    auto x = rows<std::uint8_t>(v, type_of(sec), s.explain_row, n);
    if (!x) return tl::unexpected(x.error());
    if (PersistOutcome ok = copy_enum(*x, kMaxRoute, type_of(sec), *out); !ok) return tl::unexpected(ok.error());
  }
  auto flags = rows<std::uint32_t>(v, type_of(Section::ExplainFlags), s.explain_row, n);
  if (!flags) return tl::unexpected(flags.error());
  r.flags.assign(flags->begin(), flags->end());
  auto status = rows<std::uint8_t>(v, type_of(Section::ExplainStatus), s.explain_row, n);
  if (!status) return tl::unexpected(status.error());
  if (PersistOutcome ok = copy_enum(*status, kMaxStatus, type_of(Section::ExplainStatus), r.status); !ok) {
    return tl::unexpected(ok.error());
  }
  r.total = v.totals[a.local];
  r.by_uid = buckets_of(v.by_uid.subspan(static_cast<std::size_t>(s.bucket_uid_begin), s.n_bucket_uid));
  r.by_expiry =
      buckets_of(v.by_expiry.subspan(static_cast<std::size_t>(s.bucket_expiry_begin), s.n_bucket_expiry));
  r.excluded_count = s.excluded_count;
  r.excluded_premium = s.excluded_premium;
  r.n_pivot_groups = s.n_pivot_groups;
  r.n_pivot_edge_fallback = s.n_pivot_edge_fallback;
  r.rate_move_without_rho = (s.flags & kStepRateMoveWithoutRho) != 0u;
  r.explain_seconds = s.explain_seconds;
  return r;
}

[[nodiscard]] PersistResult<HistoricalVarResult> var_of(const ReaderCore &c, std::size_t index) {
  if (index >= c.var_ref.size()) return persist_error(S::NotFound);
  const auto [s, l] = c.var_ref[index];
  const SegmentView &v = c.segs[s];
  const RiskRunVar &row = v.vars[l];
  HistoricalVarResult out;
  out.frames.reserve(static_cast<std::size_t>(row.frame_count));
  for (const RiskRunVarFrame &f :
       v.var_frames.subspan(static_cast<std::size_t>(row.frame_begin), static_cast<std::size_t>(row.frame_count))) {
    out.frames.push_back(HistoricalVarFrame{f.base_ts_ns, f.shifted_ts_ns, f.totals, f.n_legs_resolved,
                                            f.n_legs_unresolved, f.admitted != 0u});
  }
  out.points.reserve(static_cast<std::size_t>(row.point_count));
  for (const RiskRunVarPoint &p :
       v.var_points.subspan(static_cast<std::size_t>(row.point_begin), static_cast<std::size_t>(row.point_count))) {
    out.points.push_back(HistoricalVarPoint{p.confidence, p.value_at_risk, p.expected_shortfall,
                                            static_cast<std::size_t>(p.tail_scenarios), p.tail_mean});
  }
  out.n_admitted = static_cast<std::size_t>(row.n_admitted);
  out.reference_pv = row.reference_pv;
  return out;
}

[[nodiscard]] std::span<const double> frame_of(const ReaderCore &c, RiskFrameColumn column,
                                               std::size_t step) noexcept {
  const auto col = static_cast<std::uint32_t>(column);
  const auto a = c.at(step);
  if (a.step == nullptr || col >= kRiskFrameColumnCount || (a.step->flags & kStepHasFrame) == 0u) return {};
  auto r = rows<double>(*a.seg, type_of(Section::FrameF64) + col,
                        a.step->frame_index * a.seg->meta.n_unique, a.seg->meta.n_unique);
  return r ? *r : std::span<const double>{};
}

[[nodiscard]] std::span<const double> explain_of(const ReaderCore &c, RiskExplainColumn column,
                                                 std::size_t step) noexcept {
  const auto col = static_cast<std::uint32_t>(column);
  const auto a = c.at(step);
  if (a.step == nullptr || col >= kRiskExplainColumnCount || (a.step->flags & kStepHasExplain) == 0u) {
    return {};
  }
  auto r = rows<double>(*a.seg, type_of(Section::ExplainF64) + col, a.step->explain_row,
                        a.step->explain_rows);
  return r ? *r : std::span<const double>{};
}

} // namespace

std::string segment_key(std::uint64_t ordinal) {
  std::array<char, 24> buf{};
  const int n = std::snprintf(buf.data(), buf.size(), "%012llu", static_cast<unsigned long long>(ordinal));
  return std::string(buf.data(), n > 0 ? static_cast<std::size_t>(n) : 0u);
}

// ── Reader ────────────────────────────────────────────────────────────────────

struct Reader::Impl {
  ReaderCore core;
};

Reader::Reader(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}
Reader::Reader(Reader &&) noexcept = default;
Reader &Reader::operator=(Reader &&) noexcept = default;
Reader::~Reader() = default;

PersistResult<Reader> Reader::open(std::string_view dir, const PersistOpenOptions &options) {
  auto core = open_core(dir, options);
  if (!core) return tl::unexpected(core.error());
  return Reader{std::make_unique<Impl>(Impl{std::move(*core)})};
}

std::size_t Reader::n_steps() const noexcept { return impl_ ? impl_->core.step_ref.size() : 0u; }
std::size_t Reader::n_segments() const noexcept { return impl_ ? impl_->core.segs.size() : 0u; }
std::size_t Reader::n_var() const noexcept { return impl_ ? impl_->core.var_ref.size() : 0u; }
// Precondition (documented, asserted by every caller's bound): segment < n_segments().
const RiskRunMeta &Reader::meta(std::size_t segment) const noexcept { return impl_->core.segs[segment].meta; }
const PersistFile &Reader::segment(std::size_t segment) const noexcept {
  return impl_->core.segs[segment].file;
}
const RiskRunStep *Reader::step(std::size_t step) const noexcept {
  return impl_ ? impl_->core.at(step).step : nullptr;
}
std::span<const RiskRunLeg> Reader::legs() const noexcept {
  return impl_ && !impl_->core.segs.empty() ? impl_->core.segs.front().legs : std::span<const RiskRunLeg>{};
}
std::span<const std::uint64_t> Reader::leg_ids() const noexcept {
  return impl_ && !impl_->core.segs.empty() ? impl_->core.segs.front().leg_ids
                                            : std::span<const std::uint64_t>{};
}
std::span<const RiskRunUnique> Reader::unique() const noexcept {
  return impl_ && !impl_->core.segs.empty() ? impl_->core.segs.front().unique
                                            : std::span<const RiskRunUnique>{};
}
std::span<const std::uint32_t> Reader::uids() const noexcept {
  return impl_ && !impl_->core.segs.empty() ? impl_->core.segs.front().uids
                                            : std::span<const std::uint32_t>{};
}
std::span<const double> Reader::frame(RiskFrameColumn column, std::size_t step) const noexcept {
  return impl_ ? frame_of(impl_->core, column, step) : std::span<const double>{};
}
std::span<const double> Reader::explain(RiskExplainColumn column, std::size_t step) const noexcept {
  return impl_ ? explain_of(impl_->core, column, step) : std::span<const double>{};
}
const ExplainTotals *Reader::totals(std::size_t step) const noexcept {
  if (!impl_) return nullptr;
  const auto a = impl_->core.at(step);
  if (a.step == nullptr || (a.step->flags & kStepHasExplain) == 0u) return nullptr;
  return &a.seg->totals[a.local];
}
PersistResult<RiskFrame> Reader::materialize_frame(std::size_t step) const {
  if (!impl_) return persist_error(S::InvalidArgument);
  return materialize_frame_of(impl_->core, step);
}
PersistResult<ExplainResult> Reader::materialize_explain(std::size_t step) const {
  if (!impl_) return persist_error(S::InvalidArgument);
  return materialize_explain_of(impl_->core, step);
}
PersistResult<HistoricalVarResult> Reader::var(std::size_t index) const {
  if (!impl_) return persist_error(S::InvalidArgument);
  return var_of(impl_->core, index);
}

// ── Compaction ────────────────────────────────────────────────────────────────

namespace {

template <class T> [[nodiscard]] std::vector<T> rows_from_bytes(std::span<const std::byte> b) {
  std::vector<T> out(b.size() / sizeof(T));
  if (!out.empty()) std::memcpy(out.data(), b.data(), out.size() * sizeof(T));
  return out;
}

template <class T> void append_rows(std::vector<std::byte> &dst, const std::vector<T> &rows_) {
  const auto b = std::as_bytes(std::span<const T>{rows_});
  dst.insert(dst.end(), b.begin(), b.end());
}

template <std::size_t N> void put_name(std::array<char, N> &a, std::string_view s) noexcept {
  a.fill('\0');
  std::copy_n(s.begin(), std::min(s.size(), N - 1), a.begin());
}

// Replace the run's manifest with ONE entry for `image` (the merged segment), under
// the directory lock, refusing if anything was published since `seen` was read. The
// manifest record layout and fingerprint are PersistDirectory's (persist_file.hpp);
// PersistDirectory::publish can only add or replace one key, never drop the others,
// which is why this one rewrite lives here.
[[nodiscard]] PersistOutcome publish_compacted(std::string_view dir,
                                               std::span<const PersistManifestEntry> seen,
                                               std::span<const std::byte> image, std::uint32_t records) {
  const fs::path root{std::string(dir)};
  auto lock = atx::vol::detail::WriterLock::acquire((root / std::string(kPersistLockFileName)).string());
  if (!lock) {
    return persist_error(lock.error().code() == ErrorCode::Unavailable ? S::Busy : S::IoError);
  }
  auto now = PersistDirectory::open(dir);
  if (!now) return tl::unexpected(now.error());
  if (now->entries().size() != seen.size() ||
      !std::equal(seen.begin(), seen.end(), now->entries().begin(),
                  [](const PersistManifestEntry &a, const PersistManifestEntry &b) {
                    return a.seq == b.seq && a.file_crc32c == b.file_crc32c;
                  })) {
    return persist_error(S::SegmentMismatch, kPersistManifestEntriesSection);
  }
  std::uint64_t seq = 1;
  for (const PersistManifestEntry &e : seen) seq = std::max(seq, e.seq + 1);
  const std::string key = segment_key(0);
  std::string name;
  std::error_code ec;
  for (std::uint32_t probe = 0; probe < (1u << 16); ++probe, ++seq) {
    auto candidate = persist_file_name(PersistKind::RiskRun, key, seq);
    if (!candidate) return tl::unexpected(candidate.error());
    if (!fs::exists(root / *candidate, ec)) {
      name = std::move(*candidate);
      break;
    }
  }
  if (name.empty()) return persist_error(S::IoError);
  if (PersistOutcome ok = publish_persist_file((root / name).string(), image); !ok) return ok;

  PersistHeader h{};
  PersistFooter ft{};
  std::memcpy(&h, image.data(), sizeof h);
  std::memcpy(&ft, image.data() + image.size() - sizeof ft, sizeof ft);
  PersistManifestEntry e{};
  e.seq = seq;
  put_name(e.file_name, name);
  put_name(e.key, key);
  e.file_size = h.file_size;
  e.file_crc32c = ft.file_crc32c;
  e.header_crc32c = h.header_crc32c;
  e.kind = h.kind;
  e.records = records;
  e.schema_fingerprint = h.schema_fingerprint;
  e.source_fingerprint = h.source_fingerprint;
  e.ts_min_ns = h.ts_min_ns;
  e.ts_max_ns = h.ts_max_ns;
  const PersistSectionInput in{kPersistManifestEntriesSection, 1, 0, PersistDType::Pod,
                               static_cast<std::uint32_t>(sizeof(PersistManifestEntry)), true,
                               PersistCodec::None, PersistFilter::None, 3,
                               std::as_bytes(std::span{&e, 1})};
  PersistFileInfo info;
  info.kind = PersistKind::Manifest;
  info.schema_fingerprint = persist_manifest_fingerprint();
  info.ts_min_ns = e.ts_min_ns;
  info.ts_max_ns = e.ts_max_ns;
  auto manifest = build_persist_file(std::span{&in, 1}, info);
  if (!manifest) return tl::unexpected(manifest.error());
  return persist_detail::replace_file((root / std::string(kPersistManifestFileName)).string(), *manifest);
}

[[nodiscard]] bool is_book_slot(std::size_t i) noexcept {
  return i == slot(Section::BookLegs) || i == slot(Section::BookUnique) || i == slot(Section::BookUids) ||
         i == slot(Section::LegIds);
}

} // namespace
} // namespace risk_run

// ── RiskRunWriter ─────────────────────────────────────────────────────────────

struct RiskRunWriter::Impl {
  risk_run::WriterCore core;
};

RiskRunWriter::RiskRunWriter(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}
RiskRunWriter::RiskRunWriter(RiskRunWriter &&) noexcept = default;
RiskRunWriter &RiskRunWriter::operator=(RiskRunWriter &&) noexcept = default;
RiskRunWriter::~RiskRunWriter() = default;

PersistResult<RiskRunWriter> RiskRunWriter::create(std::string_view dir, const PreparedRiskBook &book,
                                                   const RiskRunOptions &options) {
  if (dir.empty() || options.segment_bytes == 0u) return persist_error(PersistStatus::InvalidArgument);
  // A run directory has one writer for its whole life: refuse one that already holds
  // a run (a resumed run would need the book identity check of the reader).
  auto existing = PersistDirectory::open(dir);
  if (existing) return persist_error(PersistStatus::AlreadyExists);
  if (existing.error().status != PersistStatus::NotFound) return tl::unexpected(existing.error());
  auto captured = risk_run::capture_book(book);
  if (!captured) return tl::unexpected(captured.error());
  auto impl = std::make_unique<Impl>();
  impl->core.dir = std::string(dir);
  impl->core.options = options;
  impl->core.book = std::move(*captured);
  return RiskRunWriter{std::move(impl)};
}

PersistOutcome RiskRunWriter::append_frame(const RiskFrame &frame) {
  if (!impl_) return persist_error(PersistStatus::InvalidArgument);
  return impl_->core.append_frame(frame);
}
PersistOutcome RiskRunWriter::append_explain(const ExplainResult &result) {
  if (!impl_) return persist_error(PersistStatus::InvalidArgument);
  return impl_->core.append_explain(result);
}
PersistOutcome RiskRunWriter::append_var(const HistoricalVarResult &result) {
  if (!impl_) return persist_error(PersistStatus::InvalidArgument);
  return impl_->core.append_var(result);
}
PersistOutcome RiskRunWriter::finish() {
  if (!impl_) return persist_error(PersistStatus::InvalidArgument);
  return impl_->core.finish();
}

// ── RiskRunArchive ────────────────────────────────────────────────────────────

struct RiskRunArchive::Impl {
  risk_run::Reader reader;
};

RiskRunArchive::RiskRunArchive(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}
RiskRunArchive::RiskRunArchive(RiskRunArchive &&) noexcept = default;
RiskRunArchive &RiskRunArchive::operator=(RiskRunArchive &&) noexcept = default;
RiskRunArchive::~RiskRunArchive() = default;

PersistResult<RiskRunArchive> RiskRunArchive::open(std::string_view dir, const PersistOpenOptions &options) {
  auto reader = risk_run::Reader::open(dir, options);
  if (!reader) return tl::unexpected(reader.error());
  return RiskRunArchive{std::make_unique<Impl>(Impl{std::move(*reader)})};
}

std::size_t RiskRunArchive::n_steps() const noexcept { return impl_ ? impl_->reader.n_steps() : 0u; }
std::span<const double> RiskRunArchive::frame(RiskFrameColumn column, std::size_t step) const noexcept {
  return impl_ ? impl_->reader.frame(column, step) : std::span<const double>{};
}
std::span<const double> RiskRunArchive::explain(RiskExplainColumn column, std::size_t step) const noexcept {
  return impl_ ? impl_->reader.explain(column, step) : std::span<const double>{};
}
const ExplainTotals *RiskRunArchive::totals(std::size_t step) const noexcept {
  return impl_ ? impl_->reader.totals(step) : nullptr;
}
std::span<const std::uint64_t> RiskRunArchive::leg_ids() const noexcept {
  return impl_ ? impl_->reader.leg_ids() : std::span<const std::uint64_t>{};
}

// ── compact_risk_run ──────────────────────────────────────────────────────────

PersistResult<std::size_t> compact_risk_run(std::string_view dir, PersistCodec codec, int level) {
  using namespace risk_run; // NOLINT(google-build-using-namespace): this TU's own detail
  if (codec != PersistCodec::None && codec != PersistCodec::Zstd) return persist_error(S::InvalidArgument);
  if (codec == PersistCodec::Zstd && (level < 1 || level > 19)) return persist_error(S::InvalidArgument);
  auto seen = PersistDirectory::open(dir);
  if (!seen) return tl::unexpected(seen.error());
  // Read every byte, verified, into memory (Read mode: no mapping is held on a file
  // the manifest is about to stop naming).
  auto reader = Reader::open(dir, PersistOpenOptions{false, PersistVerify::Full, false, 0});
  if (!reader) return tl::unexpected(reader.error());
  const std::size_t n_segments = reader->n_segments();

  std::array<std::vector<std::byte>, kSectionCount> merged;
  std::uint64_t frames = 0;
  std::uint64_t rows_ = 0;
  std::uint64_t buid = 0;
  std::uint64_t bexp = 0;
  std::uint64_t vframes = 0;
  std::uint64_t vpoints = 0;
  SegmentParts p;
  p.ts_min_ns = std::numeric_limits<std::int64_t>::max();
  p.ts_max_ns = std::numeric_limits<std::int64_t>::min();
  for (std::size_t s = 0; s < n_segments; ++s) {
    const PersistFile &f = reader->segment(s);
    const RiskRunMeta &m = reader->meta(s);
    if (m.n_steps > 0u) {
      p.ts_min_ns = std::min(p.ts_min_ns, f.header().ts_min_ns);
      p.ts_max_ns = std::max(p.ts_max_ns, f.header().ts_max_ns);
    }
    for (std::size_t i = 0; i < kSectionCount; ++i) {
      if (i == slot(Section::Meta) || (is_book_slot(i) && s > 0)) continue;
      auto b = f.section(kSchema[i].type);
      if (!b) return tl::unexpected(b.error());
      if (i == slot(Section::Steps)) {
        std::vector<RiskRunStep> steps = rows_from_bytes<RiskRunStep>(*b);
        for (RiskRunStep &st : steps) {
          if (st.frame_index != kNone64) st.frame_index += frames;
          if (st.explain_row != kNone64) st.explain_row += rows_;
          st.bucket_uid_begin += buid;
          st.bucket_expiry_begin += bexp;
        }
        append_rows(merged[i], steps);
      } else if (i == slot(Section::VarIndex)) {
        std::vector<RiskRunVar> vars = rows_from_bytes<RiskRunVar>(*b);
        for (RiskRunVar &v : vars) {
          v.frame_begin += vframes;
          v.point_begin += vpoints;
        }
        append_rows(merged[i], vars);
      } else {
        merged[i].insert(merged[i].end(), b->begin(), b->end());
      }
    }
    frames += m.n_frames;
    rows_ += m.n_explain_rows;
    buid += f.entry(type_of(Section::BucketsByUid))->element_count;
    bexp += f.entry(type_of(Section::BucketsByExpiry))->element_count;
    vframes += f.entry(type_of(Section::VarFrames))->element_count;
    vpoints += f.entry(type_of(Section::VarPoints))->element_count;
  }
  const RiskRunMeta &first = reader->meta(0);
  RiskRunMeta &m = p.meta;
  m = first;
  m.first_step = 0;
  m.n_steps = reader->n_steps();
  m.first_var = 0;
  m.n_var = reader->n_var();
  m.segment_ordinal = 0;
  m.n_frames = frames;
  m.n_explain_rows = rows_;
  if (m.n_steps == 0u) {
    p.ts_min_ns = 0;
    p.ts_max_ns = 0;
  }
  for (std::size_t i = 0; i < kSectionCount; ++i) p.bytes[i] = merged[i];
  auto image = build_segment(p, codec, codec == PersistCodec::None ? 3 : level, 4);
  if (!image) return tl::unexpected(image.error());
  const auto records = static_cast<std::uint32_t>(
      std::min<std::uint64_t>(m.n_steps, std::numeric_limits<std::uint32_t>::max()));
  if (PersistOutcome ok = publish_compacted(dir, seen->entries(), *image, records); !ok) {
    return tl::unexpected(ok.error());
  }
  return n_segments;
}

} // namespace atx::vol
