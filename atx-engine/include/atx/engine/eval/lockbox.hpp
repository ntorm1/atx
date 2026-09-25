#pragma once

// atx::engine::eval — Lockbox reservation + seal (S4.4b). The POINT-IN-TIME
// boundary that the S4.5 mine -> gate -> admit -> combine -> book pipeline runs
// strictly UPSTREAM of, and that S8.2 opens EXACTLY ONCE.
//
// ===========================================================================
//  What this header is
// ===========================================================================
//  A research process that selects, combines, and books alphas against ALL of
//  its history has no held-out judge: the final book has, transitively, seen
//  every date it is scored on. The lockbox fixes this structurally. reserve_
//  lockbox carves the TERMINAL contiguous most-recent `frac` of a panel's dates
//  as a sealed lockbox, inserts an EMBARGO gap (max label horizon + delay via
//  LockboxEmbargo, E-17; or the legacy cpcv.hpp width / an explicit length)
//  immediately before it to defeat label leakage across the boundary, and hands
//  back a SealedPanel that exposes — to every upstream stage — ONLY the visible
//  region [0, lockbox_begin - embargo_len). Any read into the sealed region
//  [lockbox_begin, T) is a contract violation: the Result accessor returns
//  Err(PermissionDenied) and the trapping accessor aborts (ATX_ASSERT). This is
//  the seal — nothing upstream of S8 may read past it.
//
//  SealedPanel itself has NO open / unseal accessor. The only way past the seal
//  is open_lockbox (bottom of this header): a single-use, pre-committed,
//  content-address-bound open that is recorded in an audit sink before any
//  held-out date is returned. Anything else would make the seal advisory.
//
// ===========================================================================
//  Determinism (load-bearing) — content-addressed, NO RNG
// ===========================================================================
//  The reservation is a PURE function of (panel, frac, embargo_len). The content-
//  address is a STABLE digest (this header's own FNV-1a/splitmix64 StableHasher,
//  never std::hash / atx::core::hash, which are not stable across processes or
//  toolchains) over the panel's shape, the reservation geometry, and the panel's
//  field-column bytes (date-major) — so the SAME panel under the SAME geometry
//  reproduces a BYTE-IDENTICAL seal (same boundaries, same address) in any
//  process on any build, while a different panel OR a different geometry shifts
//  the address. No RNG, no clock, no address-dependence anywhere. reserve_lockbox
//  is a COLD path (once per research engine), so the visible-panel copy is
//  acceptable.

#include <cmath>   // std::ceil, std::isfinite
#include <cstdint> // (digest seed bytes)
#include <cstring> // std::memcpy (StableHasher word loads)
#include <filesystem>    // FileLockboxAudit
#include <limits>        // std::numeric_limits (embargo overflow guard)
#include <span>    // std::span
#include <string>  // std::string (field-name re-enumeration)
#include <unordered_set> // opened content addresses / date digests
#include <vector>  // std::vector

#include "atx/core/error.hpp" // atx::core::Result, Ok, Err, ErrorCode
#include "atx/core/macro.hpp" // ATX_ASSERT
#include "atx/core/types.hpp" // atx::f64, atx::u64, atx::usize

#include "atx/engine/alpha/panel.hpp" // alpha::Panel, alpha::FieldId
#include "atx/engine/eval/cpcv.hpp"   // eval::CpcvConfig (embargo-width source)

namespace atx::engine::eval {

// ===========================================================================
//  SealedReservation — the lockbox boundary metadata + identity.
//
//  Trivial aggregate (Rule of Zero); owns nothing. S4.5 consumes the visible_len
//  bound; S8.2 consumes lockbox_begin + embargo_len to open the held-out region.
//
//    dates           : the full panel's date count T.
//    instruments      : the panel's instrument count (unchanged by the carve).
//    lockbox_begin    : first date of the sealed lockbox == T - floor(frac*T).
//                       The lockbox is [lockbox_begin, T) (terminal, contiguous).
//    embargo_len      : the embargo gap width (max_label_horizon + delay, the
//                       legacy cpcv.hpp ceil(h*T), or an explicit length); the
//                       dates [lockbox_begin - embargo_len,
//                       lockbox_begin) are ALSO sealed (the gap).
//    visible_len      : the visible region length == lockbox_begin - embargo_len.
//                       Upstream stages see dates [0, visible_len) ONLY.
//    content_address  : the deterministic identity (see header determinism note).
// ===========================================================================
struct SealedReservation {
  atx::usize dates{};
  atx::usize instruments{};
  atx::usize lockbox_begin{};
  atx::usize embargo_len{};
  atx::usize visible_len{};
  atx::u64 content_address{};
};

namespace detail {

// ---------------------------------------------------------------------------
//  embargo_len_from_cpcv — the embargo width in DATES from a CPCV embargo
//  fraction h and the panel length T: ceil(h * T), mirroring cpcv.hpp's
//  embargo_len = ceil(embargo * N). h <= 0 -> 0. PURE.
// ---------------------------------------------------------------------------
[[nodiscard]] inline atx::usize embargo_len_from_cpcv(atx::f64 embargo, atx::usize dates) noexcept {
  if (!(embargo > 0.0)) {
    return 0U; // also rejects NaN
  }
  const atx::f64 raw = std::ceil(embargo * static_cast<atx::f64>(dates));
  return static_cast<atx::usize>(raw);
}

// Stable digest (FNV-1a 64 over bytes, splitmix64 over 8-byte words, splitmix64
// finalizer): identical across processes, platforms and toolchains, unlike
// std::hash / atx::core::hash. Everything durable in this header — the content
// address, the per-date holdout digests, the receipt chain and the audit-log
// format — depends on that.
struct StableHasher {
  atx::u64 h{0xcbf29ce484222325ULL};
  [[nodiscard]] static constexpr atx::u64 mix(atx::u64 x) noexcept {
    atx::u64 z = x + 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31U);
  }
  void bytes(const void *p, atx::usize n) noexcept {
    const auto *c = static_cast<const unsigned char *>(p);
    for (atx::usize i = 0; i < n; ++i) {
      h ^= static_cast<atx::u64>(c[i]);
      h *= 0x100000001b3ULL;
    }
  }
  // Bulk form for column data: one bijective splitmix step per 8-byte word
  // (every input bit diffuses; no cancellation between words), bytes() for a
  // ragged tail. Little-endian word loads (x64 / arm64).
  void words(const void *p, atx::usize n) noexcept {
    const auto *c = static_cast<const unsigned char *>(p);
    atx::usize i = 0;
    for (; i + 8U <= n; i += 8U) {
      atx::u64 w = 0;
      std::memcpy(&w, c + i, sizeof(w));
      h = mix(h ^ w);
    }
    bytes(c + i, n - i);
  }
  void u64v(atx::u64 v) noexcept { bytes(&v, sizeof(v)); }
  void str(const std::string &s) noexcept {
    u64v(static_cast<atx::u64>(s.size())); // length-prefixed: no ambiguity
    bytes(s.data(), s.size());
  }
  [[nodiscard]] atx::u64 finish() const noexcept { return mix(h); }
};

// ---------------------------------------------------------------------------
//  content_address — a StableHasher digest folding the panel SHAPE, the
//  reservation GEOMETRY, and the panel's field-column bytes (date-major).
//  Deterministic and stable across processes/builds: the same panel + geometry
//  reproduces it byte-identically; a different panel OR geometry shifts it. No
//  RNG. The field bytes make it a true content-address (two panels with
//  identical shape but different prices get distinct seals). NOTE: because it
//  includes the geometry it identifies a RESERVATION, not the held-out data —
//  single-use of the data is enforced on holdout_date_digests (below).
// ---------------------------------------------------------------------------
[[nodiscard]] inline atx::u64 content_address(const alpha::Panel &panel, atx::usize lockbox_begin,
                                              atx::usize embargo_len) {
  StableHasher h;
  h.bytes("ATXLBXC1", 8U);
  h.u64v(static_cast<atx::u64>(panel.dates()));
  h.u64v(static_cast<atx::u64>(panel.instruments()));
  h.u64v(static_cast<atx::u64>(lockbox_begin));
  h.u64v(static_cast<atx::u64>(embargo_len));
  h.u64v(static_cast<atx::u64>(panel.num_fields()));
  for (atx::usize f = 0; f < panel.num_fields(); ++f) {
    const std::span<const atx::f64> col = panel.field_all(static_cast<alpha::FieldId>(f));
    h.words(col.data(), col.size_bytes());
  }
  return h.finish();
}

// ---------------------------------------------------------------------------
//  holdout_date_digests — one GEOMETRY-FREE content digest per held-out date
//  t in [begin, end): a StableHasher over (instrument count, field count, and
//  every field's cross-section bytes at t). These — not the content address —
//  are the single-use key: a date whose cross-section was already returned by
//  an open can never be returned again, however the panel is re-reserved
//  (different embargo, different frac, a window shifted by one date, the panel
//  extended with new dates). Dates whose every cell is non-finite carry no
//  information and are skipped (so padding rows shared by unrelated panels do
//  not collide). Changing the panel's field set or instrument set changes every
//  digest (it is then treated as a different dataset). Never returns 0.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::vector<atx::u64>
holdout_date_digests(const alpha::Panel &panel, atx::usize begin, atx::usize end) {
  const atx::usize insts = panel.instruments();
  const atx::usize n_fields = panel.num_fields();
  std::vector<atx::u64> out;
  out.reserve(end > begin ? end - begin : 0U);
  for (atx::usize t = begin; t < end; ++t) {
    StableHasher h;
    h.bytes("ATXLBXD1", 8U);
    h.u64v(static_cast<atx::u64>(insts));
    h.u64v(static_cast<atx::u64>(n_fields));
    bool informative = false;
    for (atx::usize f = 0; f < n_fields; ++f) {
      const std::span<const atx::f64> row =
          panel.field_all(static_cast<alpha::FieldId>(f)).subspan(t * insts, insts);
      for (const atx::f64 v : row) {
        informative = informative || std::isfinite(v);
      }
      h.words(row.data(), row.size_bytes());
    }
    if (informative) {
      const atx::u64 d = h.finish();
      out.push_back(d == 0U ? 1U : d);
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
//  build_visible_panel — rebuild a real alpha::Panel over the visible date
//  prefix [0, visible_len). Copies each field column's visible cells (date-major)
//  and the visible universe-mask prefix, re-enumerating field names via
//  Panel::field_name. Returns a self-contained Panel upstream stages consume with
//  the ordinary accessor surface. PRECONDITION (caller-validated): visible_len
//  in (0, dates].
// ---------------------------------------------------------------------------
[[nodiscard]] inline atx::core::Result<alpha::Panel> build_visible_panel(const alpha::Panel &panel,
                                                                         atx::usize visible_len) {
  const atx::usize insts = panel.instruments();
  const atx::usize n_fields = panel.num_fields();
  const atx::usize vis_cells = visible_len * insts;

  std::vector<std::string> names;
  names.reserve(n_fields);
  std::vector<std::vector<atx::f64>> cols;
  cols.reserve(n_fields);
  for (atx::usize f = 0; f < n_fields; ++f) {
    names.emplace_back(panel.field_name(f));
    const std::span<const atx::f64> full = panel.field_all(static_cast<alpha::FieldId>(f));
    cols.emplace_back(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(vis_cells));
  }

  // Reconstruct the visible universe-mask prefix from in_universe (Panel exposes
  // no raw mask). All-in-universe panels reproduce an all-1 prefix.
  std::vector<std::uint8_t> universe(vis_cells, std::uint8_t{0});
  for (atx::usize t = 0; t < visible_len; ++t) {
    for (atx::usize j = 0; j < insts; ++j) {
      universe[t * insts + j] = panel.in_universe(t, j) ? std::uint8_t{1} : std::uint8_t{0};
    }
  }
  return alpha::Panel::create(visible_len, insts, std::move(names), std::move(cols),
                              std::move(universe));
}

// ---------------------------------------------------------------------------
//  slice_panel — rebuild a real alpha::Panel over the CONTIGUOUS date range
//  [d0, d1) (the HOLDOUT terminal slice for OOS validation). Mirrors
//  build_visible_panel but over an ARBITRARY date window rather than the prefix:
//  the panel is date-major (panel.hpp), so a date range [d0, d1) is the
//  contiguous cell range [d0*N, d1*N) of every field column. Copies each field
//  column's cells in that range, slices the universe mask, and re-enumerates the
//  field names via field_name(i). Returns a self-contained Panel with dates() ==
//  d1 - d0. PRECONDITION (caller-validated): d0 < d1 <= panel.dates().
// ---------------------------------------------------------------------------
[[nodiscard]] inline atx::core::Result<alpha::Panel>
slice_panel(const alpha::Panel &panel, atx::usize d0, atx::usize d1) {
  const atx::usize insts = panel.instruments();
  const atx::usize n_fields = panel.num_fields();
  const atx::usize span_dates = d1 - d0;
  const atx::usize c0 = d0 * insts; // first cell of date d0 (date-major)
  const atx::usize c1 = d1 * insts; // one-past-last cell of date d1-1

  std::vector<std::string> names;
  names.reserve(n_fields);
  std::vector<std::vector<atx::f64>> cols;
  cols.reserve(n_fields);
  for (atx::usize f = 0; f < n_fields; ++f) {
    names.emplace_back(panel.field_name(f));
    const std::span<const atx::f64> full = panel.field_all(static_cast<alpha::FieldId>(f));
    cols.emplace_back(full.begin() + static_cast<std::ptrdiff_t>(c0),
                      full.begin() + static_cast<std::ptrdiff_t>(c1));
  }

  // Reconstruct the universe-mask slice for dates [d0, d1) (Panel exposes no raw
  // mask). All-in-universe panels reproduce an all-1 slice.
  std::vector<std::uint8_t> universe(span_dates * insts, std::uint8_t{0});
  for (atx::usize t = 0; t < span_dates; ++t) {
    for (atx::usize j = 0; j < insts; ++j) {
      universe[t * insts + j] = panel.in_universe(d0 + t, j) ? std::uint8_t{1} : std::uint8_t{0};
    }
  }
  return alpha::Panel::create(span_dates, insts, std::move(names), std::move(cols),
                              std::move(universe));
}

} // namespace detail

// ===========================================================================
//  SealedPanel — the sealed reservation: a full panel + a visible bound.
//
//  Holds the visible sub-Panel (over [0, visible_len)) that upstream stages
//  consume, plus the reservation metadata. The guarded accessors gate every read
//  on the seal: field_cross_section returns Err(PermissionDenied) for a sealed
//  date (and Err(OutOfRange) past T); field_cross_section_or_trap aborts on a
//  sealed read (ATX_ASSERT — the death-test form of the same precondition). NO
//  open/unseal API (S8.2 owns the single open). Value type (Rule of Zero); the
//  visible Panel is owned by value, so spans it hands out are valid for the
//  SealedPanel's lifetime.
// ===========================================================================
class SealedPanel {
public:
  // Built only via reserve_lockbox (the validated factory). Aggregating ctor.
  SealedPanel(alpha::Panel visible, SealedReservation res) noexcept
      : visible_{std::move(visible)}, res_{res} {}

  // The reservation boundary metadata + identity.
  [[nodiscard]] const SealedReservation &reservation() const noexcept { return res_; }

  // The visible sub-Panel over [0, visible_len) — a real Panel upstream stages
  // (mine / fitness / gate / combine) consume with the ordinary accessor surface.
  [[nodiscard]] const alpha::Panel &visible() const noexcept { return visible_; }

  // Guarded cross-section read (Result form). A date in the sealed region
  // [visible_len, lockbox_begin) U [lockbox_begin, T) -> Err(PermissionDenied)
  // (the seal); a date >= T -> Err(OutOfRange); otherwise the visible Panel's
  // cross-section. `field` must resolve in the visible Panel (caller-validated).
  [[nodiscard]] atx::core::Result<std::span<const atx::f64>>
  field_cross_section(alpha::FieldId field, alpha::DateIdx date) const {
    if (date >= res_.dates) {
      return atx::core::Err(atx::core::ErrorCode::OutOfRange,
                            "SealedPanel: date past the panel end");
    }
    if (date >= res_.visible_len) {
      return atx::core::Err(atx::core::ErrorCode::PermissionDenied,
                            "SealedPanel: read into the sealed lockbox / embargo region");
    }
    return atx::core::Ok(visible_.field_cross_section(field, date));
  }

  // Trapping cross-section read (ATX_ASSERT form). A sealed/out-of-range read
  // ABORTS (the PIT seal nothing upstream may cross). Use the Result form to
  // recover; this form is the fail-loud precondition for a stage that must never
  // read sealed dates. `field`/`date` are otherwise the visible Panel's contract.
  [[nodiscard]] std::span<const atx::f64> field_cross_section_or_trap(alpha::FieldId field,
                                                                      alpha::DateIdx date) const {
    ATX_ASSERT(date < res_.visible_len); // seal: no read at/after visible_len
    return visible_.field_cross_section(field, date);
  }

private:
  alpha::Panel visible_;  // the visible sub-Panel over [0, visible_len)
  SealedReservation res_; // boundary metadata + content-address
};

// ===========================================================================
//  reserve_lockbox (explicit embargo length).
//
//  Carve the terminal contiguous most-recent `frac` of `panel`'s dates as the
//  lockbox: lockbox_begin = T - floor(frac*T). Insert an embargo gap of
//  `embargo_len` dates immediately before it; the visible region is
//  [0, lockbox_begin - embargo_len). Returns Err(InvalidArgument) when:
//    * frac not in (0, 1) (frac<=0, or frac>=1 carving the whole panel);
//    * floor(frac*T) == 0 (the lockbox would hold no date);
//    * embargo_len >= lockbox_begin (the gap underflows / empties the visible
//      region) — equivalently visible_len would be 0 or negative.
//  PURE, deterministic, NO RNG. The content-address round-trips (header note).
// ===========================================================================
[[nodiscard]] inline atx::core::Result<SealedPanel>
reserve_lockbox(const alpha::Panel &panel, atx::f64 frac, atx::usize embargo_len) {
  const atx::usize T = panel.dates();
  // frac must lie strictly in (0, 1): a 0-date or whole-panel lockbox is invalid.
  if (!(frac > 0.0) || !(frac < 1.0)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "reserve_lockbox: frac must lie in (0, 1)");
  }
  const atx::usize lockbox_dates = static_cast<atx::usize>(static_cast<atx::f64>(T) * frac);
  if (lockbox_dates == 0U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "reserve_lockbox: frac too small to carve a lockbox date");
  }
  const atx::usize lockbox_begin = T - lockbox_dates;
  // The embargo gap must not underflow or empty the visible region.
  if (embargo_len >= lockbox_begin) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "reserve_lockbox: embargo + lockbox leave no visible region");
  }
  const atx::usize visible_len = lockbox_begin - embargo_len;

  ATX_TRY(alpha::Panel visible, detail::build_visible_panel(panel, visible_len));

  SealedReservation res;
  res.dates = T;
  res.instruments = panel.instruments();
  res.lockbox_begin = lockbox_begin;
  res.embargo_len = embargo_len;
  res.visible_len = visible_len;
  res.content_address = detail::content_address(panel, lockbox_begin, embargo_len);
  return atx::core::Ok(SealedPanel{std::move(visible), res});
}

// ===========================================================================
//  reserve_lockbox (embargo width from a CpcvConfig).
//
//  The embargo width derives from the CPCV embargo fraction h x T (cpcv.hpp
//  embargo_len = ceil(h * N), §0.9), so the lockbox gap matches the cross-
//  validation embargo the rest of the eval spine uses. Forwards to the explicit-
//  length overload with embargo_len = ceil(cfg.embargo * T).
// ===========================================================================
[[nodiscard]] inline atx::core::Result<SealedPanel>
reserve_lockbox(const alpha::Panel &panel, atx::f64 frac, const CpcvConfig &cfg) {
  const atx::usize embargo_len = detail::embargo_len_from_cpcv(cfg.embargo, panel.dates());
  return reserve_lockbox(panel, frac, embargo_len);
}

// ===========================================================================
//  reserve_lockbox (default frac 0.20, default embargo from the CpcvConfig
//  default §0.9). LEGACY (EmbargoRule::CpcvFractionV1): hold out the terminal
//  20% with the ⌈0.01·T⌉ CPCV embargo, which is not tied to any label horizon
//  (E-17). Kept so frozen reservations re-derive; new code declares its label
//  horizon via the LockboxEmbargo overloads below. PURE.
// ===========================================================================
[[nodiscard]] inline atx::core::Result<SealedPanel> reserve_lockbox(const alpha::Panel &panel) {
  return reserve_lockbox(panel, 0.20, CpcvConfig{});
}

// ===========================================================================
//  Label-horizon embargo (E-17).
//
//  The embargo exists so that no label computed from the visible region reads
//  a return inside the held-out region. A label with forward horizon h, formed
//  from a signal at date t and traded `delay` dates later, reads returns up to
//  t + delay + h; so the gap must hold at least max_label_horizon + delay
//  dates — independent of the panel length. The V1 rule ⌈fraction·T⌉ is 18
//  dates at T = 1750 (too wide for h = 1, too narrow for h = 63) and grows
//  with the panel for no reason.
//
//    LabelHorizonV2 (default): embargo_len = max_label_horizon + delay.
//      max_label_horizon must be >= 1 (the horizon must be declared).
//    CpcvFractionV1: embargo_len = ⌈cpcv_fraction·T⌉ (the legacy width).
// ===========================================================================
enum class EmbargoRule : atx::u8 {
  CpcvFractionV1 = 0, // legacy: ceil(cpcv_fraction * T)
  LabelHorizonV2 = 1, // default: max_label_horizon + delay
};

struct LockboxEmbargo {
  EmbargoRule rule{EmbargoRule::LabelHorizonV2};
  atx::usize max_label_horizon{}; // longest forward-label horizon, in dates (V2, >= 1)
  atx::usize delay{};             // signal-to-trade delay, in dates (V2)
  atx::f64 cpcv_fraction{0.01};   // V1 only (the CpcvConfig default)
};

// The embargo width in dates for a panel of `dates` dates. Err(InvalidArgument)
// for an undeclared horizon (V2, max_label_horizon == 0), a width that would
// overflow, or an unknown rule. PURE.
[[nodiscard]] inline atx::core::Result<atx::usize> lockbox_embargo_len(const LockboxEmbargo &e,
                                                                       atx::usize dates) {
  switch (e.rule) {
  case EmbargoRule::CpcvFractionV1:
    return detail::embargo_len_from_cpcv(e.cpcv_fraction, dates);
  case EmbargoRule::LabelHorizonV2:
    if (e.max_label_horizon == 0U) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "lockbox_embargo_len: declare max_label_horizon (>= 1)");
    }
    if (e.delay > std::numeric_limits<atx::usize>::max() - e.max_label_horizon) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "lockbox_embargo_len: horizon + delay overflows");
    }
    return e.max_label_horizon + e.delay;
  }
  return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                        "lockbox_embargo_len: unknown EmbargoRule");
}

// reserve_lockbox with the embargo derived from the label horizon (E-17).
[[nodiscard]] inline atx::core::Result<SealedPanel>
reserve_lockbox(const alpha::Panel &panel, atx::f64 frac, const LockboxEmbargo &embargo) {
  ATX_TRY(const atx::usize embargo_len, lockbox_embargo_len(embargo, panel.dates()));
  return reserve_lockbox(panel, frac, embargo_len);
}

// ===========================================================================
//  reserve_window — reserve an ARBITRARY contiguous holdout window
//  [holdout_begin, holdout_begin + holdout_len) with an embargo gap of
//  `embargo_len` dates immediately before it; the visible (train) region is
//  [0, holdout_begin - embargo_len). Generalizes reserve_lockbox (which is the
//  terminal special case holdout_begin == T - holdout_len). PURE, deterministic,
//  NO RNG.
//
//  Validation:
//    * holdout_len > 0 (the holdout must hold at least one date).
//    * holdout_begin + holdout_len <= T (window fits inside the panel).
//    * embargo_len < holdout_begin (visible region is non-empty; equivalently
//      holdout_begin - embargo_len >= 1).
//  Returns Err(InvalidArgument) when any condition is violated.
//
//  Byte-identity invariant: for the terminal args
//    reserve_window(panel, T - floor(frac*T), floor(frac*T), embargo_len)
//  produces a SealedPanel field-for-field equal to
//    reserve_lockbox(panel, frac, embargo_len)
//  because both compute lockbox_begin = T - floor(frac*T) and call the same
//  detail::build_visible_panel and detail::content_address helpers.
// ===========================================================================
[[nodiscard]] inline atx::core::Result<SealedPanel>
reserve_window(const alpha::Panel &panel, atx::usize holdout_begin, atx::usize holdout_len,
               atx::usize embargo_len) {
  const atx::usize T = panel.dates();
  if (holdout_len == 0U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "reserve_window: holdout_len must be > 0");
  }
  if (holdout_begin + holdout_len > T) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "reserve_window: holdout window [holdout_begin, holdout_begin+holdout_len) "
                          "extends past the panel end");
  }
  // embargo_len < holdout_begin ensures visible_len = holdout_begin - embargo_len >= 1.
  if (embargo_len >= holdout_begin) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "reserve_window: embargo_len >= holdout_begin leaves no visible region");
  }
  const atx::usize visible_len = holdout_begin - embargo_len;

  ATX_TRY(alpha::Panel visible, detail::build_visible_panel(panel, visible_len));

  SealedReservation res;
  res.dates = T;
  res.instruments = panel.instruments();
  res.lockbox_begin = holdout_begin;
  res.embargo_len = embargo_len;
  res.visible_len = visible_len;
  res.content_address = detail::content_address(panel, holdout_begin, embargo_len);
  return atx::core::Ok(SealedPanel{std::move(visible), res});
}

// reserve_window with the embargo derived from the label horizon (E-17).
[[nodiscard]] inline atx::core::Result<SealedPanel>
reserve_window(const alpha::Panel &panel, atx::usize holdout_begin, atx::usize holdout_len,
               const LockboxEmbargo &embargo) {
  ATX_TRY(const atx::usize embargo_len, lockbox_embargo_len(embargo, panel.dates()));
  return reserve_window(panel, holdout_begin, holdout_len, embargo_len);
}

// ===========================================================================
//  open_lockbox — the SINGLE audited open of a sealed reservation (S8.2).
//
//  The seal above is only as strong as the open is rare. open_lockbox makes
//  the open a one-shot, recorded event:
//
//   * Single-use per held-out DATE. The audit sink remembers the content
//     address of every opened reservation AND a geometry-free digest of every
//     held-out date it returned (detail::holdout_date_digests). An open is
//     refused with Err(AlreadyExists) when its reservation was opened before
//     OR when any date in its holdout was already returned by an earlier open
//     — so re-reserving the same data with a different embargo, a different
//     frac, a window shifted by a date, or a panel extended with new dates
//     cannot re-open held-out history. A holdout made entirely of NEW dates
//     (e.g. a later lockbox over data that arrived after the first open) is
//     still openable. A durable sink (FileLockboxAudit) makes that hold across
//     processes and across concurrent writers of one log.
//   * Bound to the sealed data. The caller supplies the full panel; its
//     content address under the reservation geometry must equal the sealed
//     one, else Err(PermissionDenied). A different panel cannot be opened
//     under this seal.
//   * Pre-committed. OpenRequest::candidate_hash (the frozen model / book
//     config being judged) must be non-zero, else Err(InvalidArgument): the
//     candidate is fixed BEFORE the held-out data is seen, and the receipt
//     binds the two together.
//   * Tamper-evident. The receipt hash is a stable digest of every receipt
//     field (date digests included) plus the previous receipt's hash (a hash
//     chain), so EDITING any committed receipt — or any line of the durable
//     log — is detectable (verify_receipt / FileLockboxAudit::open).
//     Removing whole trailing receipts leaves a valid chain prefix, which the
//     log alone cannot reveal: export lockbox_chain_head(sink) OUTSIDE the
//     log (a run manifest) after every open and check a reopened log with
//     verify_lockbox_chain_head(receipts, anchor) (bottom of this header).
//   * Refusals consume nothing: the receipt is committed only after every
//     check passes (the sink re-checks single-use atomically at commit), and
//     the held-out slice is returned only after the commit succeeded.
//
//  The holdout returned is [lockbox_begin, lockbox_begin + holdout_len), or
//  [lockbox_begin, T) when holdout_len == 0 (the reserve_lockbox case).
// ===========================================================================

struct OpenRequest {
  std::string purpose;       // why the lockbox is being opened (audited)
  std::string requester;     // who/what is opening it (audited)
  atx::u64 candidate_hash{}; // commitment to the frozen candidate; must be != 0
  atx::usize holdout_len{};  // 0 == through the panel end
};

struct LockboxReceipt {
  atx::u64 sequence{};        // 0-based position in the audit chain
  atx::u64 content_address{}; // the opened reservation's identity
  atx::u64 candidate_hash{};
  atx::usize holdout_begin{};
  atx::usize holdout_end{}; // exclusive
  // Geometry-free digest of each informative held-out date (the single-use key).
  std::vector<atx::u64> date_digests;
  std::string purpose;   // sanitized: no tab / CR / LF
  std::string requester; // sanitized: no tab / CR / LF
  atx::u64 prev_receipt_hash{};
  atx::u64 receipt_hash{};
};

namespace detail {

[[nodiscard]] inline std::string sanitize_audit_text(std::string s) {
  for (char &c : s) {
    if (c == '\t' || c == '\n' || c == '\r') {
      c = ' ';
    }
  }
  return s;
}

} // namespace detail

// The receipt hash over every field except receipt_hash itself.
[[nodiscard]] inline atx::u64 lockbox_receipt_hash(const LockboxReceipt &r) noexcept {
  detail::StableHasher h;
  h.bytes("ATXLBX2", 7U);
  h.u64v(r.sequence);
  h.u64v(r.content_address);
  h.u64v(r.candidate_hash);
  h.u64v(static_cast<atx::u64>(r.holdout_begin));
  h.u64v(static_cast<atx::u64>(r.holdout_end));
  h.u64v(static_cast<atx::u64>(r.date_digests.size()));
  for (const atx::u64 d : r.date_digests) {
    h.u64v(d);
  }
  h.str(r.purpose);
  h.str(r.requester);
  h.u64v(r.prev_receipt_hash);
  return h.finish();
}

[[nodiscard]] inline bool verify_receipt(const LockboxReceipt &r) noexcept {
  return lockbox_receipt_hash(r) == r.receipt_hash;
}

namespace detail {

// The in-memory state every sink keeps: the receipt chain plus the opened
// reservation addresses and opened date digests (the single-use key set).
class LockboxLedger {
public:
  [[nodiscard]] bool is_opened(atx::u64 content_address) const {
    return opened_.find(content_address) != opened_.end();
  }
  [[nodiscard]] bool any_date_opened(std::span<const atx::u64> date_digests) const {
    for (const atx::u64 d : date_digests) {
      if (dates_.find(d) != dates_.end()) {
        return true;
      }
    }
    return false;
  }
  [[nodiscard]] atx::u64 next_sequence() const noexcept {
    return static_cast<atx::u64>(receipts_.size());
  }
  [[nodiscard]] atx::u64 last_receipt_hash() const noexcept {
    return receipts_.empty() ? 0U : receipts_.back().receipt_hash;
  }
  // Err(AlreadyExists) when the draft's reservation or any of its dates is opened.
  [[nodiscard]] atx::core::Status check_fresh(const LockboxReceipt &draft) const {
    if (is_opened(draft.content_address) || any_date_opened(draft.date_digests)) {
      return atx::core::Err(atx::core::ErrorCode::AlreadyExists,
                            "lockbox audit: this lockbox (or one of its held-out dates) "
                            "was already opened");
    }
    return atx::core::Ok();
  }
  // Chain the draft onto the ledger head: sequence, prev hash, receipt hash.
  [[nodiscard]] LockboxReceipt seal(LockboxReceipt draft) const {
    draft.sequence = next_sequence();
    draft.prev_receipt_hash = last_receipt_hash();
    draft.receipt_hash = lockbox_receipt_hash(draft);
    return draft;
  }
  // True when `r` is the valid next link of this chain.
  [[nodiscard]] bool links(const LockboxReceipt &r) const noexcept {
    return verify_receipt(r) && r.sequence == next_sequence() &&
           r.prev_receipt_hash == last_receipt_hash();
  }
  void ingest(LockboxReceipt r) {
    opened_.insert(r.content_address);
    dates_.insert(r.date_digests.begin(), r.date_digests.end());
    receipts_.push_back(std::move(r));
  }
  [[nodiscard]] const std::vector<LockboxReceipt> &receipts() const noexcept { return receipts_; }

private:
  std::unordered_set<atx::u64> opened_;
  std::unordered_set<atx::u64> dates_;
  std::vector<LockboxReceipt> receipts_;
};

} // namespace detail

// ===========================================================================
//  LockboxAuditSink — where opens are recorded. commit() is the commit point
//  and must be ATOMIC with respect to every other writer of the same backing
//  store: it re-checks single-use (reservation address AND every date digest;
//  Err(AlreadyExists) on overlap), chains the draft onto the CURRENT head
//  (sequence, prev hash, receipt hash), persists it, and returns the committed
//  receipt. After a successful commit, is_opened / any_date_opened are true
//  for the receipt's address / dates.
// ===========================================================================
class LockboxAuditSink {
public:
  virtual ~LockboxAuditSink() = default;
  [[nodiscard]] virtual bool is_opened(atx::u64 content_address) const = 0;
  [[nodiscard]] virtual bool any_date_opened(std::span<const atx::u64> date_digests) const = 0;
  [[nodiscard]] virtual atx::u64 next_sequence() const = 0;
  [[nodiscard]] virtual atx::u64 last_receipt_hash() const = 0;
  [[nodiscard]] virtual atx::core::Result<LockboxReceipt> commit(LockboxReceipt draft) = 0;
};

// Process-local sink (tests, single-run research engines). Single-threaded.
class InMemoryLockboxAudit final : public LockboxAuditSink {
public:
  [[nodiscard]] bool is_opened(atx::u64 content_address) const override {
    return ledger_.is_opened(content_address);
  }
  [[nodiscard]] bool any_date_opened(std::span<const atx::u64> date_digests) const override {
    return ledger_.any_date_opened(date_digests);
  }
  [[nodiscard]] atx::u64 next_sequence() const override { return ledger_.next_sequence(); }
  [[nodiscard]] atx::u64 last_receipt_hash() const override { return ledger_.last_receipt_hash(); }
  [[nodiscard]] atx::core::Result<LockboxReceipt> commit(LockboxReceipt draft) override {
    ATX_TRY_VOID(ledger_.check_fresh(draft));
    LockboxReceipt r = ledger_.seal(std::move(draft));
    ledger_.ingest(r);
    return atx::core::Ok(std::move(r));
  }
  [[nodiscard]] const std::vector<LockboxReceipt> &receipts() const noexcept {
    return ledger_.receipts();
  }

private:
  detail::LockboxLedger ledger_;
};

// Durable, append-only, one tab-separated line per receipt:
//   ATXLBX2 seq addr cand begin end prev hash digests purpose requester
// (hex numbers; `digests` is a comma-separated hex list, "-" when empty).
// open() replays and VERIFIES the whole chain (hash + prev link + sequence);
// any mismatch is Err(ParseError) — the log was edited. A final line without
// its newline is a torn append (crash) and is ignored and truncated.
//
// Concurrency: open() and commit() hold an exclusive OS file lock on the log
// (LockFileEx / flock; released automatically if the process dies). commit()
// first ingests every line other handles / processes appended since this
// handle last looked, re-checks single-use against that fresh state, then
// appends and flushes to stable storage (FlushFileBuffers / fsync). Two
// handles on one path therefore cannot both open the same held-out dates, and
// the chain never forks. A log that SHRANK under a handle is Err(ParseError).
// Truncation of whole trailing lines is not detectable from the log alone;
// anchor it with lockbox_chain_head / verify_lockbox_chain_head.
class FileLockboxAudit final : public LockboxAuditSink {
public:
  [[nodiscard]] static atx::core::Result<FileLockboxAudit>
  open(const std::filesystem::path &path);

  [[nodiscard]] bool is_opened(atx::u64 content_address) const override {
    return ledger_.is_opened(content_address);
  }
  [[nodiscard]] bool any_date_opened(std::span<const atx::u64> date_digests) const override {
    return ledger_.any_date_opened(date_digests);
  }
  [[nodiscard]] atx::u64 next_sequence() const override { return ledger_.next_sequence(); }
  [[nodiscard]] atx::u64 last_receipt_hash() const override { return ledger_.last_receipt_hash(); }
  [[nodiscard]] atx::core::Result<LockboxReceipt> commit(LockboxReceipt draft) override;
  [[nodiscard]] const std::vector<LockboxReceipt> &receipts() const noexcept {
    return ledger_.receipts();
  }

private:
  FileLockboxAudit() = default;
  struct Io; // src/eval/lockbox_audit.cpp: locked file I/O + line codec
  friend struct Io;

  std::filesystem::path path_;
  detail::LockboxLedger ledger_;
  atx::u64 synced_len_{}; // bytes of the log already verified + ingested
};

struct LockboxOpening {
  alpha::Panel holdout;   // the held-out slice, dates [holdout_begin, holdout_end)
  LockboxReceipt receipt; // already committed to the sink
};

// Consumes `sealed` (the visible handle is spent by the open). See the block
// comment above for the full contract and error codes.
[[nodiscard]] inline atx::core::Result<LockboxOpening>
open_lockbox(SealedPanel &&sealed, const alpha::Panel &full, const OpenRequest &req,
             LockboxAuditSink &sink) {
  using atx::core::Err;
  using atx::core::ErrorCode;
  const SealedPanel spent{std::move(sealed)};
  const SealedReservation &res = spent.reservation();
  if (req.candidate_hash == 0U) {
    return Err(ErrorCode::InvalidArgument,
               "open_lockbox: candidate_hash must commit to the frozen candidate");
  }
  if (sink.is_opened(res.content_address)) {
    return Err(ErrorCode::AlreadyExists, "open_lockbox: this lockbox was already opened");
  }
  if (full.dates() != res.dates || full.instruments() != res.instruments ||
      detail::content_address(full, res.lockbox_begin, res.embargo_len) !=
          res.content_address) {
    return Err(ErrorCode::PermissionDenied, "open_lockbox: panel does not match the seal");
  }
  const atx::usize begin = res.lockbox_begin;
  const atx::usize end = req.holdout_len == 0U ? res.dates : begin + req.holdout_len;
  if (end > res.dates || end <= begin) {
    return Err(ErrorCode::InvalidArgument, "open_lockbox: holdout window past the panel end");
  }
  LockboxReceipt draft;
  draft.content_address = res.content_address;
  draft.candidate_hash = req.candidate_hash;
  draft.holdout_begin = begin;
  draft.holdout_end = end;
  draft.date_digests = detail::holdout_date_digests(full, begin, end);
  if (sink.any_date_opened(draft.date_digests)) {
    return Err(ErrorCode::AlreadyExists,
               "open_lockbox: held-out dates were already returned by an earlier open");
  }
  draft.purpose = detail::sanitize_audit_text(req.purpose);
  draft.requester = detail::sanitize_audit_text(req.requester);
  ATX_TRY(alpha::Panel holdout, detail::slice_panel(full, begin, end));
  // The sink re-checks single-use atomically and chains the receipt.
  ATX_TRY(LockboxReceipt receipt, sink.commit(std::move(draft)));
  return atx::core::Ok(LockboxOpening{std::move(holdout), std::move(receipt)});
}

// ===========================================================================
//  Chain head export (tamper evidence outside the log).
//
//  lockbox_chain_head(sink) = {receipts, head}: the number of committed
//  receipts and the last receipt hash (0 when none). Store it OUTSIDE the
//  audit log (a run manifest, a separate WORM location) after each open.
//  verify_lockbox_chain_head(receipts, anchor) checks a (re)loaded receipt
//  chain against it: fewer receipts than anchored (trailing receipts deleted)
//  or a different hash at position anchor.receipts - 1 (a receipt replaced)
//  is Err(ParseError); receipts committed after the anchor are accepted. The
//  receipts themselves are chain-verified by FileLockboxAudit::open.
// ===========================================================================
struct LockboxChainHead {
  atx::u64 receipts{}; // committed receipts the head covers
  atx::u64 head{};     // receipt_hash of the last of them (0 when receipts == 0)
  friend bool operator==(const LockboxChainHead &, const LockboxChainHead &) = default;
};

[[nodiscard]] inline LockboxChainHead lockbox_chain_head(const LockboxAuditSink &sink) {
  return LockboxChainHead{sink.next_sequence(), sink.last_receipt_hash()};
}

[[nodiscard]] inline atx::core::Status
verify_lockbox_chain_head(std::span<const LockboxReceipt> receipts,
                          const LockboxChainHead &anchor) {
  if (receipts.size() < anchor.receipts) {
    return atx::core::Err(atx::core::ErrorCode::ParseError,
                          "lockbox audit: fewer receipts than the anchored chain head "
                          "(receipts were removed)");
  }
  const atx::u64 at = anchor.receipts == 0U
                          ? 0U
                          : receipts[static_cast<atx::usize>(anchor.receipts - 1U)].receipt_hash;
  if (at != anchor.head) {
    return atx::core::Err(atx::core::ErrorCode::ParseError,
                          "lockbox audit: the receipt chain does not match its anchored head");
  }
  return atx::core::Ok();
}

} // namespace atx::engine::eval
