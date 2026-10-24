// MarketState archive (kind MarketState, `.atxms`): encoder, zero-copy reader and the
// bench-selection bucket. Spec: docs/storage/PERSIST_FORMAT.md "MarketState kind";
// design wave3-persist D10 / D14.
//
// The reader maps the file, validates every row range, every side byte and the mark
// order, rebuilds only what cannot be mapped (UnderlierState rows, YieldCurves, views
// over the embedded ATXVSA2 records) and hands `MarketState::create` spans INTO THE
// MAPPING. All of that lives in one heap-pinned Impl, so a moved archive keeps every
// address the state borrows.

#include "atx/vol/api/persist/state_archive.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/api/storage/surface_archive.hpp"
#include "persist/state_archive_format.hpp"

namespace atx::vol {
namespace {

namespace sf = state_format;
using MS = MarketStateSection;
using sf::sec;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::size_t kExtras = std::size(sf::kExtrasSections);

template <class T> [[nodiscard]] std::span<const std::byte> bytes_of(const std::vector<T> &v) {
  return std::as_bytes(std::span<const T>{v});
}

template <std::size_t N> void put_text(std::array<char, N> &a, std::string_view s) noexcept {
  a.fill('\0');
  std::copy_n(s.begin(), std::min(s.size(), N - 1), a.begin());
}

// NUL-terminated, zero-padded fixed text; nullopt when malformed.
template <std::size_t N>
[[nodiscard]] std::optional<std::string_view> get_text(const std::array<char, N> &a) noexcept {
  const auto end = std::find(a.begin(), a.end(), '\0');
  if (end == a.end() || std::any_of(end, a.end(), [](char c) { return c != '\0'; })) {
    return std::nullopt;
  }
  return std::string_view{a.data(), static_cast<std::size_t>(end - a.begin())};
}

[[nodiscard]] std::span<const double> extras_column(const MarkExtrasColumns &e,
                                                    std::size_t k) noexcept {
  switch (k) {
  case 0: return e.bid;
  case 1: return e.ask;
  case 2: return e.vendor_gamma;
  case 3: return e.vendor_vega;
  case 4: return e.vendor_theta;
  case 5: return e.vendor_T;
  default: return e.uprc;
  }
}

[[nodiscard]] std::string surface_symbol(std::uint32_t uid) { return "U" + std::to_string(uid); }

// ── Encoder ──────────────────────────────────────────────────────────────────

// Everything the encoder writes, owned until build_persist_file has framed it.
struct Encoded {
  sf::Meta meta{};
  std::vector<sf::UnderlierRow> rows;
  std::string strings;
  std::vector<ExpiryCarry> carry;
  std::vector<DividendEvent> divs;
  std::vector<sf::YieldPillar> pillars;
  std::vector<double> strike, sigma, vprice, vdelta;
  std::vector<std::int64_t> expiry;
  std::vector<std::uint8_t> side;
  std::array<std::vector<double>, kExtras> extras;
  std::vector<sf::SurfaceIndexRow> surface_index;
  std::vector<std::byte> surface_archive;
  std::vector<sf::UidTableRow> uid_table;
};

// The stored curve's pillars, proven: a curve rebuilt from them must reproduce the
// stored curve's discount factor bit-for-bit at every pillar and between them.
[[nodiscard]] PersistOutcome add_yield(const MarketStateWriteInput &in, const UnderlierState &u,
                                       sf::UnderlierRow &row, Encoded &out) {
  if (u.yield == nullptr) return {};
  const MarketStateStorage &s = *in.storage;
  std::size_t k = 0;
  while (k < s.yield_curves.size() && &s.yield_curves[k] != u.yield) ++k;
  // The pillars are the arrays the curve was created from, so the reader's
  // YieldCurve::create over them rebuilds it bit-equal (rates_curve
  // YieldCurve.Pillars_RebuildBitEqualCurve). A curve outside the storage, or an empty
  // (default-constructed) one, has nothing to rebuild from.
  const YieldCurvePillarSpans p =
      k < s.yield_curves.size() ? s.yield_curves[k].pillars() : YieldCurvePillarSpans{};
  if (p.t_years.empty() || p.zero_rates.size() != p.t_years.size()) {
    return persist_error(PersistStatus::InvalidArgument, sec(MS::YieldPillars), u.uid);
  }
  row.curve_begin = static_cast<std::uint32_t>(out.pillars.size());
  row.curve_count = static_cast<std::uint32_t>(p.t_years.size());
  for (std::size_t i = 0; i < p.t_years.size(); ++i) {
    out.pillars.push_back({p.t_years[i], p.zero_rates[i]});
  }
  return {};
}

// Append one uid's marks (and its extras rows) to the global columns.
[[nodiscard]] PersistOutcome add_marks(const MarketStateWriteInput &in, std::size_t mi,
                                       sf::UnderlierRow &row, Encoded &out) {
  const MarketStateStorage::MarksColumns &m = in.storage->marks[mi];
  const std::size_t n = m.sigma.size();
  row.mark_flags |= sf::kRowHasMarks;
  row.marks_begin = out.sigma.size();
  row.marks_count = n;
  out.strike.insert(out.strike.end(), m.strike.begin(), m.strike.end());
  out.expiry.insert(out.expiry.end(), m.expiry_ns.begin(), m.expiry_ns.end());
  for (const Side sd : m.side) out.side.push_back(static_cast<std::uint8_t>(sd));
  out.sigma.insert(out.sigma.end(), m.sigma.begin(), m.sigma.end());
  if (out.meta.has_vendor_price != 0u) {
    if (m.vendor_price.empty()) {
      out.vprice.insert(out.vprice.end(), n, kNaN);
    } else {
      row.mark_flags |= sf::kRowVendorPrice;
      out.vprice.insert(out.vprice.end(), m.vendor_price.begin(), m.vendor_price.end());
    }
  }
  if (out.meta.has_vendor_delta != 0u) {
    if (m.vendor_delta.empty()) {
      out.vdelta.insert(out.vdelta.end(), n, kNaN);
    } else {
      row.mark_flags |= sf::kRowVendorDelta;
      out.vdelta.insert(out.vdelta.end(), m.vendor_delta.begin(), m.vendor_delta.end());
    }
  }
  if (out.meta.has_extras == 0u) return {};
  for (std::size_t k = 0; k < kExtras; ++k) {
    const std::span<const double> col = extras_column(in.extras[mi], k);
    if (!col.empty() && col.size() != n) {
      return persist_error(PersistStatus::InvalidArgument, sec(sf::kExtrasSections[k]), mi);
    }
    if (col.empty()) {
      out.extras[k].insert(out.extras[k].end(), n, kNaN);
    } else {
      out.extras[k].insert(out.extras[k].end(), col.begin(), col.end());
    }
  }
  return {};
}

// Embed the owned surfaces as ONE verbatim ATXVSA2 image (the existing encoder: there
// is no second surface encoder), indexed by uid; symbol "U<uid>" (collision-free).
[[nodiscard]] PersistOutcome add_surfaces(const MarketStateWriteInput &in, Encoded &out) {
  const std::span<const SurfaceRef> refs =
      in.surfaces.empty() ? std::span<const SurfaceRef>{in.storage->surfaces} : in.surfaces;
  if (refs.empty()) return {};
  std::vector<std::pair<std::uint32_t, const PricedSurface *>> owned;
  for (const SurfaceRef &r : refs) {
    if (!r.valid() || r.owned() == nullptr) {
      return persist_error(PersistStatus::InvalidArgument, sec(MS::SurfaceArchive));
    }
    owned.emplace_back(r.uid(), r.owned());
  }
  std::sort(owned.begin(), owned.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });
  std::vector<std::string> symbols;
  symbols.reserve(owned.size());
  std::vector<SurfaceArchiveItem> items;
  for (std::size_t i = 0; i < owned.size(); ++i) {
    if (i > 0 && owned[i].first == owned[i - 1].first) {
      return persist_error(PersistStatus::InvalidArgument, sec(MS::SurfaceArchive), owned[i].first);
    }
    const auto row =
        std::lower_bound(out.rows.begin(), out.rows.end(), owned[i].first,
                         [](const sf::UnderlierRow &r, std::uint32_t u) { return r.uid < u; });
    if (row == out.rows.end() || row->uid != owned[i].first) {
      return persist_error(PersistStatus::InvalidArgument, sec(MS::SurfaceArchive), owned[i].first);
    }
    row->surface_slot = static_cast<std::uint32_t>(i);
    symbols.push_back(surface_symbol(owned[i].first));
    sf::SurfaceIndexRow idx{};
    idx.uid = owned[i].first;
    idx.symbol_offset = static_cast<std::uint32_t>(out.strings.size());
    idx.symbol_length = static_cast<std::uint32_t>(symbols.back().size());
    out.strings += symbols.back();
    out.surface_index.push_back(idx);
    items.push_back(SurfaceArchiveItem{symbols.back(), owned[i].second, std::nullopt});
  }
  auto image = write_surface_archive_v2(items);
  if (!image) return persist_error(PersistStatus::InvalidArgument, sec(MS::SurfaceArchive));
  out.surface_archive = std::move(*image);
  return {};
}

void fill_meta(const MarketStateWriteInput &in, sf::Meta &m) {
  const MarketStateStorage &s = *in.storage;
  m.ts_ns = s.ts_ns;
  m.time_convention = static_cast<std::uint8_t>(s.time.convention);
  m.conventions_version = in.conventions_version;
  m.vt_alpha = s.time.vol_time.alpha;
  m.vt_trading_hours_per_year = s.time.vol_time.trading_hours_per_year;
  m.vt_nontrading_hours_per_year = s.time.vol_time.nontrading_hours_per_year;
  m.vt_session_open_hour_et = s.time.vol_time.session_open_hour_et;
  m.vt_session_span_hours = s.time.vol_time.session_span_hours;
  if (in.report != nullptr) {
    const BucketStateReport &r = *in.report;
    m.has_report = 1;
    m.rep_n_uids = r.n_uids;
    m.rep_n_uids_dropped = r.n_uids_dropped;
    m.rep_n_marks = r.n_marks;
    m.rep_n_uids_with_divs = r.n_uids_with_divs;
    m.rep_n_expiry_rows = r.n_expiry_rows;
    m.rep_max_ddiv_spread = r.max_ddiv_spread;
    m.rep_n_duplicate_marks = r.n_duplicate_marks;
    m.rep_n_rows_bad_sigma = r.n_rows_bad_sigma;
  }
  put_text(m.date, in.date);
  put_text(m.bucket_et, in.bucket_et);
}

// Size every column once: growing them by doubling would fault in roughly twice the
// pages (the encode cost of a large state is mostly first-touch page faults).
void reserve_columns(const MarketStateStorage &s, Encoded &out) {
  std::size_t marks = 0;
  std::size_t carry = 0;
  std::size_t divs = 0;
  for (const auto &m : s.marks) marks += m.sigma.size();
  for (const UnderlierState &u : s.underliers) {
    carry += u.term_carry.size();
    divs += u.cash_divs.size();
  }
  out.rows.reserve(s.underliers.size());
  out.carry.reserve(carry);
  out.divs.reserve(divs);
  out.strike.reserve(marks);
  out.expiry.reserve(marks);
  out.side.reserve(marks);
  out.sigma.reserve(marks);
  if (out.meta.has_vendor_price != 0u) out.vprice.reserve(marks);
  if (out.meta.has_vendor_delta != 0u) out.vdelta.reserve(marks);
  if (out.meta.has_extras != 0u) {
    for (auto &col : out.extras) col.reserve(marks);
  }
}

[[nodiscard]] PersistResult<Encoded> encode_rows(const MarketStateWriteInput &in) {
  const MarketStateStorage &s = *in.storage;
  Encoded out;
  fill_meta(in, out.meta);
  out.meta.has_extras = in.extras.empty() ? 0 : 1;
  for (const auto &m : s.marks) {
    out.meta.has_vendor_price |= m.vendor_price.empty() ? 0 : 1;
    out.meta.has_vendor_delta |= m.vendor_delta.empty() ? 0 : 1;
  }
  std::vector<std::size_t> order(s.underliers.size());
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    return s.underliers[a].uid < s.underliers[b].uid;
  });
  std::vector<std::pair<std::uint32_t, std::size_t>> marks_of;
  for (std::size_t i = 0; i < s.marks.size(); ++i) marks_of.emplace_back(s.marks[i].uid, i);
  std::sort(marks_of.begin(), marks_of.end());
  reserve_columns(s, out);
  for (const std::size_t ui : order) {
    const UnderlierState &u = s.underliers[ui];
    sf::UnderlierRow row{};
    row.uid = u.uid;
    row.carry = static_cast<std::uint8_t>(u.carry);
    row.spot = u.spot;
    row.flat_rate = u.flat_rate;
    row.flat_q = u.flat_q;
    row.carry_begin = static_cast<std::uint32_t>(out.carry.size());
    row.carry_count = static_cast<std::uint32_t>(u.term_carry.size());
    out.carry.insert(out.carry.end(), u.term_carry.begin(), u.term_carry.end());
    row.divs_begin = static_cast<std::uint32_t>(out.divs.size());
    row.divs_count = static_cast<std::uint32_t>(u.cash_divs.size());
    out.divs.insert(out.divs.end(), u.cash_divs.begin(), u.cash_divs.end());
    if (PersistOutcome ok = add_yield(in, u, row, out); !ok) return tl::unexpected(ok.error());
    const std::string_view ticker = u.uid >= 1 && u.uid - 1 < in.tickers_by_uid.size()
                                        ? std::string_view{in.tickers_by_uid[u.uid - 1]}
                                        : std::string_view{};
    row.ticker_offset = static_cast<std::uint32_t>(out.strings.size());
    row.ticker_length = static_cast<std::uint32_t>(ticker.size());
    out.strings += ticker;
    const auto hit = std::lower_bound(marks_of.begin(), marks_of.end(),
                                      std::pair<std::uint32_t, std::size_t>{u.uid, 0});
    if (hit != marks_of.end() && hit->first == u.uid) {
      if (PersistOutcome ok = add_marks(in, hit->second, row, out); !ok) {
        return tl::unexpected(ok.error());
      }
    }
    if ((row.mark_flags & sf::kRowHasMarks) == 0u) row.marks_begin = out.sigma.size();
    out.rows.push_back(row);
  }
  if (PersistOutcome ok = add_surfaces(in, out); !ok) return tl::unexpected(ok.error());
  for (const std::string &t : in.tickers_by_uid) {
    out.uid_table.push_back({static_cast<std::uint32_t>(out.strings.size()),
                             static_cast<std::uint32_t>(t.size())});
    out.strings += t;
  }
  out.meta.n_underliers = out.rows.size();
  out.meta.n_marks = out.sigma.size();
  out.meta.n_term_carry = out.carry.size();
  out.meta.n_cash_divs = out.divs.size();
  out.meta.n_yield_pillars = out.pillars.size();
  out.meta.n_surfaces = out.surface_index.size();
  return out;
}

[[nodiscard]] PersistSectionInput section_of(MS type, PersistDType dtype, std::uint32_t stride,
                                             bool required, std::span<const std::byte> bytes) {
  PersistSectionInput s;
  s.type = sec(type);
  s.dtype = dtype;
  s.stride = stride;
  s.required = required;
  s.bytes = bytes;
  return s;
}

} // namespace

std::uint64_t market_state_fingerprint() noexcept { return sf::kFingerprint; }

PersistResult<std::vector<std::byte>> encode_market_state(const MarketStateWriteInput &in) {
  if (in.storage == nullptr || in.date.size() >= 16 || in.bucket_et.size() >= 8 ||
      (!in.extras.empty() && in.extras.size() != in.storage->marks.size())) {
    return persist_error(PersistStatus::InvalidArgument);
  }
  // Everything MarketState refuses (unsorted marks, bad sides, duplicate uids, ...) is
  // refused here too, before anything is written.
  if (auto st = make_market_state(*in.storage); !st) {
    return persist_error(PersistStatus::InvalidArgument);
  }
  auto enc = encode_rows(in);
  if (!enc) return tl::unexpected(enc.error());
  const Encoded &e = *enc;
  const sf::Meta meta = e.meta;
  std::vector<PersistSectionInput> s;
  s.push_back(section_of(MS::Meta, PersistDType::Pod, sizeof(sf::Meta), true,
                         std::as_bytes(std::span{&meta, 1})));
  s.push_back(section_of(MS::Underliers, PersistDType::Pod, sizeof(sf::UnderlierRow), true,
                         bytes_of(e.rows)));
  s.push_back(section_of(MS::Strings, PersistDType::Bytes, 1, true,
                         std::as_bytes(std::span<const char>{e.strings.data(), e.strings.size()})));
  s.push_back(section_of(MS::TermCarry, PersistDType::Pod, sizeof(ExpiryCarry), true,
                         bytes_of(e.carry)));
  s.push_back(section_of(MS::CashDivs, PersistDType::Pod, sizeof(DividendEvent), true,
                         bytes_of(e.divs)));
  s.push_back(section_of(MS::YieldPillars, PersistDType::Pod, sizeof(sf::YieldPillar), true,
                         bytes_of(e.pillars)));
  if (!e.uid_table.empty()) {
    s.push_back(section_of(MS::UidTable, PersistDType::Pod, sizeof(sf::UidTableRow), false,
                           bytes_of(e.uid_table)));
  }
  s.push_back(section_of(MS::Strike, PersistDType::F64, 8, true, bytes_of(e.strike)));
  s.push_back(section_of(MS::Expiry, PersistDType::I64, 8, true, bytes_of(e.expiry)));
  s.push_back(section_of(MS::Side, PersistDType::U8, 1, true, bytes_of(e.side)));
  s.push_back(section_of(MS::Sigma, PersistDType::F64, 8, true, bytes_of(e.sigma)));
  if (meta.has_vendor_price != 0u) {
    s.push_back(section_of(MS::VendorPrice, PersistDType::F64, 8, false, bytes_of(e.vprice)));
  }
  if (meta.has_vendor_delta != 0u) {
    s.push_back(section_of(MS::VendorDelta, PersistDType::F64, 8, false, bytes_of(e.vdelta)));
  }
  if (meta.has_extras != 0u) {
    for (std::size_t k = 0; k < kExtras; ++k) {
      s.push_back(section_of(sf::kExtrasSections[k], PersistDType::F64, 8, false,
                             bytes_of(e.extras[k])));
    }
  }
  if (meta.n_surfaces != 0u) {
    s.push_back(section_of(MS::SurfaceIndex, PersistDType::Pod, sizeof(sf::SurfaceIndexRow), false,
                           bytes_of(e.surface_index)));
    s.push_back(section_of(MS::SurfaceArchive, PersistDType::Bytes, 1, false, e.surface_archive));
  }
  PersistFileInfo info;
  info.kind = PersistKind::MarketState;
  info.schema_fingerprint = sf::kFingerprint;
  info.created_unix_ns = in.created_unix_ns;
  info.ts_min_ns = in.storage->ts_ns;
  info.ts_max_ns = in.storage->ts_ns;
  info.source_fingerprint = in.source_fingerprint;
  return build_persist_file(s, info, std::max(1u, in.n_threads));
}

// ── Reader ───────────────────────────────────────────────────────────────────

namespace {

// Everything an open archive holds (MarketStateArchive::Impl derives from it so the
// free helpers below can fill it; the nested name is private).
struct ArchiveCore {
  std::optional<PersistFile> file;
  sf::Meta meta{};
  std::span<const sf::UnderlierRow> rows;
  std::string_view strings;
  std::span<const double> strike, sigma, vprice, vdelta;
  std::span<const std::int64_t> expiry;
  std::span<const Side> side;
  std::array<std::span<const double>, kExtras> extras{};
  std::span<const ExpiryCarry> carry;
  std::span<const DividendEvent> divs;
  std::span<const sf::YieldPillar> pillars;
  std::span<const sf::SurfaceIndexRow> surface_index;
  std::span<const sf::UidTableRow> uid_table;
  std::vector<YieldCurve> curves;
  std::vector<std::uint32_t> curve_of_row; // index into curves, kPersistNone without one
  std::vector<UnderlierState> underliers;
  std::vector<ContractMarks> marks;
  std::optional<SurfaceArchiveV2> surface_archive;
  std::vector<PricedSurfaceView> views;
  std::vector<SurfaceRef> refs;
  MarketState state;

  [[nodiscard]] const sf::UnderlierRow *row_of(std::uint32_t uid) const noexcept {
    const auto it =
        std::lower_bound(rows.begin(), rows.end(), uid,
                         [](const sf::UnderlierRow &r, std::uint32_t u) { return r.uid < u; });
    return it == rows.end() || it->uid != uid ? nullptr : &*it;
  }
};

template <class T>
[[nodiscard]] PersistResult<std::span<const T>> column(const PersistFile &f, MS type,
                                                      std::uint64_t n) {
  auto v = f.typed<T>(sec(type));
  if (!v) return tl::unexpected(v.error());
  if (v->size() != n) return persist_error(PersistStatus::SectionStride, sec(type), v->size());
  return *v;
}

template <class T> [[nodiscard]] PersistOutcome ok_of(const PersistResult<T> &r) {
  if (r) return {};
  return tl::unexpected(r.error());
}

[[nodiscard]] PersistOutcome check_meta(const sf::Meta &m) noexcept {
  const bool flags_ok = m.time_convention <= 1u && m.has_vendor_price <= 1u &&
                        m.has_vendor_delta <= 1u && m.has_extras <= 1u && m.has_report <= 1u;
  const bool reserved_ok =
      std::all_of(m.reserved0.begin(), m.reserved0.end(), [](std::uint8_t b) { return b == 0; }) &&
      std::all_of(m.reserved1.begin(), m.reserved1.end(), [](std::uint8_t b) { return b == 0; });
  if (!flags_ok || !reserved_ok || !get_text(m.date) || !get_text(m.bucket_et) ||
      m.n_underliers > std::numeric_limits<std::uint32_t>::max()) {
    return persist_error(PersistStatus::RecordInvalid, sec(MS::Meta));
  }
  return {};
}

// Map every section the meta says exists (and refuse one it says does not).
[[nodiscard]] PersistOutcome map_sections(ArchiveCore &a) {
  const PersistFile &f = *a.file;
  const sf::Meta &m = a.meta;
  auto rows = column<sf::UnderlierRow>(f, MS::Underliers, m.n_underliers);
  auto strings = f.section(sec(MS::Strings));
  auto carry = column<ExpiryCarry>(f, MS::TermCarry, m.n_term_carry);
  auto divs = column<DividendEvent>(f, MS::CashDivs, m.n_cash_divs);
  auto pillars = column<sf::YieldPillar>(f, MS::YieldPillars, m.n_yield_pillars);
  auto strike = column<double>(f, MS::Strike, m.n_marks);
  auto expiry = column<std::int64_t>(f, MS::Expiry, m.n_marks);
  auto side = column<std::uint8_t>(f, MS::Side, m.n_marks);
  auto sigma = column<double>(f, MS::Sigma, m.n_marks);
  for (const PersistOutcome &ok : {ok_of(rows), ok_of(strings), ok_of(carry), ok_of(divs),
                                   ok_of(pillars), ok_of(strike), ok_of(expiry), ok_of(side),
                                   ok_of(sigma)}) {
    if (!ok) return ok;
  }
  a.rows = *rows;
  // SAFETY: std::byte -> char view of the string blob (char may alias any object).
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  a.strings = std::string_view{reinterpret_cast<const char *>(strings->data()), strings->size()};
  a.carry = *carry;
  a.divs = *divs;
  a.pillars = *pillars;
  a.strike = *strike;
  a.expiry = *expiry;
  a.sigma = *sigma;
  // Every side byte in {0, 1} BEFORE the column is viewed as Side (persist D3).
  for (std::size_t i = 0; i < side->size(); ++i) {
    if ((*side)[i] > 1u) return persist_error(PersistStatus::RecordInvalid, sec(MS::Side), i);
  }
  auto sides = f.typed<Side>(sec(MS::Side));
  if (!sides) return tl::unexpected(sides.error());
  a.side = *sides;
  const auto optional_col = [&](MS type, bool present,
                                std::span<const double> &out) -> PersistOutcome {
    if (!present) {
      return f.entry(sec(type)) == nullptr ? PersistOutcome{}
                                           : persist_error(PersistStatus::RecordInvalid, sec(type));
    }
    auto c = column<double>(f, type, m.n_marks);
    if (!c) return tl::unexpected(c.error());
    out = *c;
    return {};
  };
  if (PersistOutcome ok = optional_col(MS::VendorPrice, m.has_vendor_price != 0u, a.vprice); !ok) {
    return ok;
  }
  if (PersistOutcome ok = optional_col(MS::VendorDelta, m.has_vendor_delta != 0u, a.vdelta); !ok) {
    return ok;
  }
  for (std::size_t k = 0; k < kExtras; ++k) {
    if (PersistOutcome ok =
            optional_col(sf::kExtrasSections[k], m.has_extras != 0u, a.extras[k]);
        !ok) {
      return ok;
    }
  }
  if (f.entry(sec(MS::UidTable)) != nullptr) {
    auto table = f.typed<sf::UidTableRow>(sec(MS::UidTable));
    if (!table) return tl::unexpected(table.error());
    a.uid_table = *table;
  }
  if (m.n_surfaces != 0u) {
    auto idx = column<sf::SurfaceIndexRow>(f, MS::SurfaceIndex, m.n_surfaces);
    if (!idx) return tl::unexpected(idx.error());
    a.surface_index = *idx;
  } else if (f.entry(sec(MS::SurfaceIndex)) != nullptr ||
             f.entry(sec(MS::SurfaceArchive)) != nullptr) {
    return persist_error(PersistStatus::RecordInvalid, sec(MS::SurfaceIndex));
  }
  return {};
}

[[nodiscard]] bool in_range(std::uint64_t begin, std::uint64_t count, std::uint64_t size) noexcept {
  return begin <= size && count <= size - begin;
}

// Row ranges, flags and order (persist D3: bounds before any span is formed).
[[nodiscard]] PersistOutcome check_rows(const ArchiveCore &a) noexcept {
  using S = PersistStatus;
  const std::uint32_t u = sec(MS::Underliers);
  std::uint64_t cursor = 0;
  for (std::size_t i = 0; i < a.rows.size(); ++i) {
    const sf::UnderlierRow &r = a.rows[i];
    if (i > 0 && !(a.rows[i - 1].uid < r.uid)) return persist_error(S::IndexUnsorted, u, i);
    const bool marks = (r.mark_flags & sf::kRowHasMarks) != 0u;
    if (r.carry > 1u || (r.mark_flags & ~sf::kRowKnownFlags) != 0u || r.reserved0 != 0u ||
        r.reserved1 != 0u || r.reserved2 != 0u ||
        ((r.mark_flags & sf::kRowVendorPrice) != 0u && (a.meta.has_vendor_price == 0u || !marks)) ||
        ((r.mark_flags & sf::kRowVendorDelta) != 0u && (a.meta.has_vendor_delta == 0u || !marks)) ||
        (!marks && r.marks_count != 0u)) {
      return persist_error(S::RecordInvalid, u, i);
    }
    if (r.marks_begin != cursor || !in_range(r.marks_begin, r.marks_count, a.sigma.size()) ||
        !in_range(r.carry_begin, r.carry_count, a.carry.size()) ||
        !in_range(r.divs_begin, r.divs_count, a.divs.size()) ||
        !in_range(r.curve_begin, r.curve_count, a.pillars.size()) ||
        !in_range(r.ticker_offset, r.ticker_length, a.strings.size())) {
      return persist_error(S::IndexBounds, u, i);
    }
    cursor += r.marks_count;
    if (r.surface_slot != kPersistNone && (r.surface_slot >= a.surface_index.size() ||
                                           a.surface_index[r.surface_slot].uid != r.uid)) {
      return persist_error(S::IndexBounds, sec(MS::SurfaceIndex), i);
    }
  }
  if (cursor != a.sigma.size()) return persist_error(S::IndexBounds, u, cursor);
  for (std::size_t j = 0; j < a.uid_table.size(); ++j) {
    if (!in_range(a.uid_table[j].offset, a.uid_table[j].length, a.strings.size())) {
      return persist_error(S::IndexBounds, sec(MS::UidTable), j);
    }
  }
  for (std::size_t j = 0; j < a.surface_index.size(); ++j) {
    const sf::SurfaceIndexRow &s = a.surface_index[j];
    const sf::UnderlierRow *r = a.row_of(s.uid);
    if (s.reserved != 0u || r == nullptr || r->surface_slot != j ||
        !in_range(s.symbol_offset, s.symbol_length, a.strings.size())) {
      return persist_error(S::IndexBounds, sec(MS::SurfaceIndex), j);
    }
  }
  return {};
}

// Within each uid, marks ascending by (expiry, side, strike) — the order resolve()
// binary-searches (equal keys allowed, as MarketState allows them).
[[nodiscard]] PersistOutcome check_mark_order(const ArchiveCore &a) noexcept {
  for (const sf::UnderlierRow &r : a.rows) {
    const auto b = static_cast<std::size_t>(r.marks_begin);
    for (std::size_t i = b + 1; i < b + r.marks_count; ++i) {
      const std::int64_t e0 = a.expiry[i - 1];
      const std::int64_t e1 = a.expiry[i];
      const bool side_less = a.side[i] < a.side[i - 1];
      const bool strike_less = a.side[i] == a.side[i - 1] && a.strike[i] < a.strike[i - 1];
      const bool less = e1 < e0 || (e1 == e0 && (side_less || strike_less));
      if (less) return persist_error(PersistStatus::IndexUnsorted, sec(MS::Strike), i);
    }
  }
  return {};
}

[[nodiscard]] TimeSpec time_of(const sf::Meta &m) noexcept {
  TimeSpec t;
  t.convention = static_cast<TimeConvention>(m.time_convention);
  t.vol_time.alpha = m.vt_alpha;
  t.vol_time.trading_hours_per_year = m.vt_trading_hours_per_year;
  t.vol_time.nontrading_hours_per_year = m.vt_nontrading_hours_per_year;
  t.vol_time.session_open_hour_et = m.vt_session_open_hour_et;
  t.vol_time.session_span_hours = m.vt_session_span_hours;
  return t;
}

// Rebuild the owned parts and create the state over spans into the backing.
[[nodiscard]] PersistOutcome build_state(ArchiveCore &a) {
  const std::size_t n = a.rows.size();
  std::size_t n_curves = 0;
  for (const sf::UnderlierRow &r : a.rows) n_curves += r.curve_count != 0u ? 1u : 0u;
  a.curves.reserve(n_curves); // reserved first: UnderlierState::yield points into it
  a.curve_of_row.assign(n, kPersistNone);
  a.underliers.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const sf::UnderlierRow &r = a.rows[i];
    UnderlierState u;
    u.uid = r.uid;
    u.spot = r.spot;
    u.flat_rate = r.flat_rate;
    u.flat_q = r.flat_q;
    u.carry = static_cast<CarryFrom>(r.carry);
    u.term_carry = a.carry.subspan(r.carry_begin, r.carry_count);
    u.cash_divs = a.divs.subspan(r.divs_begin, r.divs_count);
    if (r.curve_count != 0u) {
      std::vector<double> t(r.curve_count), z(r.curve_count);
      for (std::uint32_t k = 0; k < r.curve_count; ++k) {
        t[k] = a.pillars[r.curve_begin + k].t_years;
        z[k] = a.pillars[r.curve_begin + k].zero_rate;
      }
      auto curve = YieldCurve::create(t, z);
      if (!curve) return persist_error(PersistStatus::RecordInvalid, sec(MS::YieldPillars), i);
      a.curve_of_row[i] = static_cast<std::uint32_t>(a.curves.size());
      a.curves.push_back(std::move(*curve));
      u.yield = &a.curves.back();
    }
    a.underliers.push_back(u);
    if ((r.mark_flags & sf::kRowHasMarks) == 0u) continue;
    const auto b = static_cast<std::size_t>(r.marks_begin);
    const auto c = static_cast<std::size_t>(r.marks_count);
    ContractMarks m;
    m.uid = r.uid;
    m.strike = a.strike.subspan(b, c);
    m.expiry_ns = a.expiry.subspan(b, c);
    m.side = a.side.subspan(b, c);
    m.sigma = a.sigma.subspan(b, c);
    if ((r.mark_flags & sf::kRowVendorPrice) != 0u) m.vendor_price = a.vprice.subspan(b, c);
    if ((r.mark_flags & sf::kRowVendorDelta) != 0u) m.vendor_delta = a.vdelta.subspan(b, c);
    a.marks.push_back(m);
  }
  return {};
}

[[nodiscard]] PersistOutcome build_surfaces(ArchiveCore &a) {
  if (a.surface_index.empty()) return {};
  auto image = a.file->section(sec(MS::SurfaceArchive));
  if (!image) return tl::unexpected(image.error());
  auto archive = SurfaceArchiveV2::open_borrowed(*image, a.file->keepalive());
  if (!archive) return persist_error(PersistStatus::RecordInvalid, sec(MS::SurfaceArchive));
  a.surface_archive.emplace(std::move(*archive));
  a.views.reserve(a.surface_index.size()); // before any ref is taken (views are move-only)
  for (std::size_t j = 0; j < a.surface_index.size(); ++j) {
    const sf::SurfaceIndexRow &s = a.surface_index[j];
    auto view = a.surface_archive->map_symbol(a.strings.substr(s.symbol_offset, s.symbol_length));
    if (!view || view->uid() != s.uid) {
      return persist_error(PersistStatus::RecordInvalid, sec(MS::SurfaceArchive), j);
    }
    a.views.push_back(std::move(*view));
  }
  a.refs.reserve(a.views.size());
  for (const PricedSurfaceView &v : a.views) a.refs.emplace_back(&v);
  return {};
}

[[nodiscard]] PersistOutcome load(ArchiveCore &a, PersistFile file) {
  a.file.emplace(std::move(file));
  if (PersistOutcome ok = a.file->check_schema(sf::kSchema); !ok) return ok;
  auto meta = a.file->typed<sf::Meta>(sec(MS::Meta));
  if (!meta) return tl::unexpected(meta.error());
  if (meta->size() != 1) {
    return persist_error(PersistStatus::RecordInvalid, sec(MS::Meta), meta->size());
  }
  a.meta = (*meta)[0];
  if (PersistOutcome ok = check_meta(a.meta); !ok) return ok;
  if (PersistOutcome ok = map_sections(a); !ok) return ok;
  if (PersistOutcome ok = check_rows(a); !ok) return ok;
  if (PersistOutcome ok = check_mark_order(a); !ok) return ok;
  if (PersistOutcome ok = build_state(a); !ok) return ok;
  if (PersistOutcome ok = build_surfaces(a); !ok) return ok;
  auto state = MarketState::create(a.meta.ts_ns, time_of(a.meta), a.underliers, a.marks, a.refs);
  if (!state) return persist_error(PersistStatus::RecordInvalid);
  a.state = std::move(*state);
  return {};
}

} // namespace

struct MarketStateArchive::Impl final : ArchiveCore {};

MarketStateArchive::MarketStateArchive(std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)} {}
MarketStateArchive::MarketStateArchive(MarketStateArchive &&) noexcept = default;
MarketStateArchive &MarketStateArchive::operator=(MarketStateArchive &&) noexcept = default;
MarketStateArchive::~MarketStateArchive() = default;

PersistResult<MarketStateArchive> MarketStateArchive::open(std::string_view path) {
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(std::filesystem::path{path}, ec);
  // An unreadable size falls through to the Full open, which reports the real refusal.
  return open(path, market_state_open_options(ec ? 0u : static_cast<std::uint64_t>(size)));
}

PersistResult<MarketStateArchive> MarketStateArchive::open(std::string_view path,
                                                           const PersistOpenOptions &options) {
  auto file = PersistFile::open(path, PersistKind::MarketState, sf::kFingerprint, options);
  if (!file) return tl::unexpected(file.error());
  auto impl = std::make_unique<Impl>();
  if (PersistOutcome ok = load(*impl, std::move(*file)); !ok) return tl::unexpected(ok.error());
  return MarketStateArchive{std::move(impl)};
}

PersistResult<MarketStateArchive> MarketStateArchive::open_bytes(std::span<const std::byte> bytes,
                                                                 std::shared_ptr<const void> owner,
                                                                 PersistVerify verify) {
  auto file = PersistFile::open_bytes(bytes, std::move(owner), PersistKind::MarketState,
                                      sf::kFingerprint, verify);
  if (!file) return tl::unexpected(file.error());
  auto impl = std::make_unique<Impl>();
  if (PersistOutcome ok = load(*impl, std::move(*file)); !ok) return tl::unexpected(ok.error());
  return MarketStateArchive{std::move(impl)};
}

const MarketState &MarketStateArchive::state() const noexcept { return impl_->state; }

std::string_view MarketStateArchive::ticker(std::uint32_t uid) const noexcept {
  const ArchiveCore &a = *impl_;
  if (const sf::UnderlierRow *r = a.row_of(uid); r != nullptr) {
    return a.strings.substr(r->ticker_offset, r->ticker_length);
  }
  if (uid == 0 || uid > a.uid_table.size()) return {};
  return a.strings.substr(a.uid_table[uid - 1].offset, a.uid_table[uid - 1].length);
}

std::uint32_t MarketStateArchive::uid_table_size() const noexcept {
  return static_cast<std::uint32_t>(impl_->uid_table.size());
}

MarkExtras MarketStateArchive::extras(std::uint32_t uid) const noexcept {
  const sf::UnderlierRow *r = impl_->row_of(uid);
  if (r == nullptr || impl_->meta.has_extras == 0u || (r->mark_flags & sf::kRowHasMarks) == 0u) {
    return {};
  }
  const auto b = static_cast<std::size_t>(r->marks_begin);
  const auto c = static_cast<std::size_t>(r->marks_count);
  const auto &x = impl_->extras;
  return MarkExtras{x[0].subspan(b, c), x[1].subspan(b, c), x[2].subspan(b, c), x[3].subspan(b, c),
                    x[4].subspan(b, c), x[5].subspan(b, c), x[6].subspan(b, c)};
}

std::uint64_t MarketStateArchive::source_fingerprint() const noexcept {
  return impl_->file->header().source_fingerprint;
}

const PersistFile &MarketStateArchive::file() const noexcept { return *impl_->file; }
bool MarketStateArchive::has_extras() const noexcept { return impl_->meta.has_extras != 0u; }
std::uint32_t MarketStateArchive::conventions_version() const noexcept {
  return impl_->meta.conventions_version;
}
std::string_view MarketStateArchive::date() const noexcept {
  return get_text(impl_->meta.date).value_or("");
}
std::string_view MarketStateArchive::bucket_et() const noexcept {
  return get_text(impl_->meta.bucket_et).value_or("");
}
std::size_t MarketStateArchive::n_marks() const noexcept {
  return static_cast<std::size_t>(impl_->meta.n_marks);
}

BucketStateReport MarketStateArchive::report() const noexcept {
  const sf::Meta &m = impl_->meta;
  BucketStateReport r;
  r.n_uids = static_cast<std::size_t>(m.rep_n_uids);
  r.n_uids_dropped = static_cast<std::size_t>(m.rep_n_uids_dropped);
  r.n_marks = static_cast<std::size_t>(m.rep_n_marks);
  r.n_uids_with_divs = static_cast<std::size_t>(m.rep_n_uids_with_divs);
  r.n_expiry_rows = static_cast<std::size_t>(m.rep_n_expiry_rows);
  r.max_ddiv_spread = m.rep_max_ddiv_spread;
  r.n_duplicate_marks = static_cast<std::size_t>(m.rep_n_duplicate_marks);
  r.n_rows_bad_sigma = static_cast<std::size_t>(m.rep_n_rows_bad_sigma);
  return r;
}

PersistResult<MarketStateStorage> MarketStateArchive::materialize() const {
  const ArchiveCore &a = *impl_;
  MarketStateStorage s;
  s.ts_ns = a.meta.ts_ns;
  s.time = time_of(a.meta);
  s.underliers = a.underliers;
  s.yield_curves = a.curves;
  s.dividend_schedules.reserve(a.rows.size());
  s.term_carries.reserve(a.rows.size());
  for (const UnderlierState &u : a.underliers) {
    s.dividend_schedules.emplace_back(u.cash_divs.begin(), u.cash_divs.end());
    s.term_carries.emplace_back(u.term_carry.begin(), u.term_carry.end());
  }
  // Every vector is final: point the rows at the copies (vector moves keep buffers).
  for (std::size_t i = 0; i < s.underliers.size(); ++i) {
    s.underliers[i].cash_divs = s.dividend_schedules[i];
    s.underliers[i].term_carry = s.term_carries[i];
    const std::uint32_t curve = a.curve_of_row[i];
    s.underliers[i].yield = curve == kPersistNone ? nullptr : &s.yield_curves[curve];
  }
  s.marks.reserve(a.marks.size());
  for (const ContractMarks &m : a.marks) {
    MarketStateStorage::MarksColumns c;
    c.uid = m.uid;
    c.strike.assign(m.strike.begin(), m.strike.end());
    c.expiry_ns.assign(m.expiry_ns.begin(), m.expiry_ns.end());
    c.side.assign(m.side.begin(), m.side.end());
    c.sigma.assign(m.sigma.begin(), m.sigma.end());
    c.vendor_price.assign(m.vendor_price.begin(), m.vendor_price.end());
    c.vendor_delta.assign(m.vendor_delta.begin(), m.vendor_delta.end());
    s.marks.push_back(std::move(c));
  }
  s.surfaces = a.refs;
  return s;
}

Result<SpiderRockBucket> selection_bucket_from_archive(const MarketStateArchive &archive) {
  const MarketState &st = archive.state();
  bool any_delta = false;
  for (const UnderlierState &u : st.underliers()) {
    const ContractMarks *m = st.marks(u.uid);
    any_delta = any_delta || (m != nullptr && !m->vendor_delta.empty());
  }
  if (!archive.has_extras() || !any_delta) {
    return Err(ErrorCode::InvalidArgument,
               "selection_bucket_from_archive: the archive carries no extras / vendor delta "
               "(the bench book selector needs bid, ask and vendor delta per row)");
  }
  SpiderRockBucket b;
  b.date = std::string(archive.date());
  b.bucket_et = std::string(archive.bucket_et());
  b.ts_ns = st.ts_ns();
  const std::size_t n = archive.n_marks();
  for (std::vector<double> *col : {&b.strike, &b.sigma, &b.vendor_price, &b.vendor_delta, &b.bid,
                                   &b.ask, &b.vendor_gamma, &b.vendor_vega, &b.vendor_theta,
                                   &b.vendor_T, &b.uprc}) {
    col->reserve(n);
  }
  // Rows in archive (uid, expiry, side, strike) order: exactly the loader's bucket order.
  for (const UnderlierState &u : st.underliers()) {
    const ContractMarks *m = st.marks(u.uid);
    if (m == nullptr) continue;
    const std::size_t k = m->size();
    const MarkExtras x = archive.extras(u.uid);
    b.uid.insert(b.uid.end(), k, u.uid);
    b.strike.insert(b.strike.end(), m->strike.begin(), m->strike.end());
    b.expiry_ns.insert(b.expiry_ns.end(), m->expiry_ns.begin(), m->expiry_ns.end());
    b.side.insert(b.side.end(), m->side.begin(), m->side.end());
    b.sigma.insert(b.sigma.end(), m->sigma.begin(), m->sigma.end());
    const auto put = [k](std::vector<double> &dst, std::span<const double> src) {
      if (src.empty()) {
        dst.insert(dst.end(), k, kNaN);
      } else {
        dst.insert(dst.end(), src.begin(), src.end());
      }
    };
    put(b.vendor_price, m->vendor_price);
    put(b.vendor_delta, m->vendor_delta);
    put(b.bid, x.bid);
    put(b.ask, x.ask);
    put(b.vendor_gamma, x.vendor_gamma);
    put(b.vendor_vega, x.vendor_vega);
    put(b.vendor_theta, x.vendor_theta);
    put(b.vendor_T, x.vendor_T);
    put(b.uprc, x.uprc);
  }
  b.rows_read = static_cast<std::int64_t>(b.sigma.size());
  return b;
}

} // namespace atx::vol
