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
//  as a sealed lockbox, inserts an EMBARGO gap (cpcv.hpp width) immediately
//  before it to defeat serial-correlation leakage across the boundary, and hands
//  back a SealedPanel that exposes — to every upstream stage — ONLY the visible
//  region [0, lockbox_begin - embargo_len). Any read into the sealed region
//  [lockbox_begin, T) is a contract violation: the Result accessor returns
//  Err(PermissionDenied) and the trapping accessor aborts (ATX_ASSERT). This is
//  the seal — nothing upstream of S8 may read past it.
//
//  THERE IS NO OPEN / UNSEAL API HERE BY DESIGN. S4.4 establishes the reservation
//  + seal as a PIT boundary; S8.2 is the single, audited site that opens the
//  lockbox exactly once for the final out-of-sample evaluation. Adding an open
//  here would make the seal advisory. S8 adds the open against this boundary
//  metadata (lockbox_begin / embargo_len) — this header deliberately ships none.
//
// ===========================================================================
//  Determinism (load-bearing) — content-addressed, NO RNG
// ===========================================================================
//  The reservation is a PURE function of (panel, frac, embargo_len). The content-
//  address is a wyhash digest over the panel's shape, the reservation geometry,
//  and the panel's field-column bytes (date-major) — so the SAME panel under the
//  SAME geometry reproduces a BYTE-IDENTICAL seal (same boundaries, same address),
//  while a different panel OR a different geometry shifts the address. No RNG, no
//  clock, no address-dependence anywhere. reserve_lockbox is a COLD path (once per
//  research engine), so the visible-panel copy is acceptable.

#include <cmath>   // std::ceil
#include <cstdint> // (digest seed bytes)
#include <span>    // std::span
#include <string>  // std::string (field-name re-enumeration)
#include <vector>  // std::vector

#include "atx/core/error.hpp" // atx::core::Result, Ok, Err, ErrorCode
#include "atx/core/hash.hpp"  // atx::core::hash_bytes, hash_combine
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
//    embargo_len      : the embargo gap width (cpcv.hpp ceil(h*T) or an explicit
//                       length); the dates [lockbox_begin - embargo_len,
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

// ---------------------------------------------------------------------------
//  content_address — a wyhash digest folding the panel SHAPE, the reservation
//  GEOMETRY, and the panel's field-column bytes (date-major). Deterministic: the
//  same panel + geometry reproduces it byte-identically; a different panel OR
//  geometry shifts it. No RNG. The field bytes make it a true content-address
//  (two panels with identical shape but different prices get distinct seals).
// ---------------------------------------------------------------------------
[[nodiscard]] inline atx::u64 content_address(const alpha::Panel &panel, atx::usize lockbox_begin,
                                              atx::usize embargo_len) {
  // Fold the shape + geometry scalars (order-sensitive) into the seed.
  std::size_t seed = atx::core::hash_combine(std::size_t{0}, panel.dates(), panel.instruments(),
                                             lockbox_begin, embargo_len, panel.num_fields());
  // Fold each field column's raw bytes (date-major) — the panel's content. Hashing
  // every cell makes the address sensitive to the actual price/feature history.
  for (atx::usize f = 0; f < panel.num_fields(); ++f) {
    const std::span<const atx::f64> col = panel.field_all(static_cast<alpha::FieldId>(f));
    const atx::u64 col_digest = atx::core::hash_bytes(col.data(), col.size_bytes());
    seed = atx::core::hash_combine(seed, col_digest);
  }
  return static_cast<atx::u64>(seed);
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
//  default §0.9). The plan-default reservation: hold out the terminal 20% with
//  the standard CPCV embargo. PURE.
// ===========================================================================
[[nodiscard]] inline atx::core::Result<SealedPanel> reserve_lockbox(const alpha::Panel &panel) {
  return reserve_lockbox(panel, 0.20, CpcvConfig{});
}

} // namespace atx::engine::eval
