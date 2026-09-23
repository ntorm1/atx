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
//  SealedPanel itself has NO open / unseal accessor. The only way past the seal
//  is open_lockbox (bottom of this header): a single-use, pre-committed,
//  content-address-bound open that is recorded in an audit sink before any
//  held-out date is returned. Anything else would make the seal advisory.
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

#include <charconv>      // std::to_chars, std::from_chars (audit log)
#include <cmath>   // std::ceil
#include <cstdint> // (digest seed bytes)
#include <filesystem>    // FileLockboxAudit
#include <fstream>       // FileLockboxAudit
#include <optional>      // parse results
#include <span>    // std::span
#include <string>  // std::string (field-name re-enumeration)
#include <string_view>   // audit-log parsing
#include <system_error>  // std::errc
#include <unordered_set> // opened content addresses
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

// ===========================================================================
//  open_lockbox — the SINGLE audited open of a sealed reservation (S8.2).
//
//  The seal above is only as strong as the open is rare. open_lockbox makes
//  the open a one-shot, recorded event:
//
//   * Single-use per reservation IDENTITY. The audit sink remembers every
//     opened content_address; a second open of the same lockbox — even via a
//     freshly re-reserved SealedPanel over the same panel and geometry — is
//     refused with Err(AlreadyExists). A durable sink (FileLockboxAudit) makes
//     that hold across processes.
//   * Bound to the sealed data. The caller supplies the full panel; its
//     content address under the reservation geometry must equal the sealed
//     one, else Err(PermissionDenied). A different panel cannot be opened
//     under this seal.
//   * Pre-committed. OpenRequest::candidate_hash (the frozen model / book
//     config being judged) must be non-zero, else Err(InvalidArgument): the
//     candidate is fixed BEFORE the held-out data is seen, and the receipt
//     binds the two together.
//   * Tamper-evident. The receipt hash is a stable digest of every receipt
//     field plus the previous receipt's hash (a hash chain), so editing any
//     committed receipt — or the durable log — is detectable
//     (verify_receipt / FileLockboxAudit::open).
//   * Refusals consume nothing: the receipt is appended only after every
//     check passes, and the held-out slice is returned only after the append
//     succeeded.
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
  std::string purpose;      // sanitized: no tab / CR / LF
  std::string requester;    // sanitized: no tab / CR / LF
  atx::u64 prev_receipt_hash{};
  atx::u64 receipt_hash{};
};

namespace detail {

// Stable digest (FNV-1a 64 + splitmix64 finalizer): identical across processes,
// unlike std::hash; the receipt chain and its log format depend on that.
struct StableHasher {
  atx::u64 h{0xcbf29ce484222325ULL};
  void bytes(const void *p, atx::usize n) noexcept {
    const auto *c = static_cast<const unsigned char *>(p);
    for (atx::usize i = 0; i < n; ++i) {
      h ^= static_cast<atx::u64>(c[i]);
      h *= 0x100000001b3ULL;
    }
  }
  void u64v(atx::u64 v) noexcept { bytes(&v, sizeof(v)); }
  void str(const std::string &s) noexcept {
    u64v(static_cast<atx::u64>(s.size())); // length-prefixed: no ambiguity
    bytes(s.data(), s.size());
  }
  [[nodiscard]] atx::u64 finish() const noexcept {
    atx::u64 z = h + 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31U);
  }
};

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
  h.bytes("ATXLBX1", 7U);
  h.u64v(r.sequence);
  h.u64v(r.content_address);
  h.u64v(r.candidate_hash);
  h.u64v(static_cast<atx::u64>(r.holdout_begin));
  h.u64v(static_cast<atx::u64>(r.holdout_end));
  h.str(r.purpose);
  h.str(r.requester);
  h.u64v(r.prev_receipt_hash);
  return h.finish();
}

[[nodiscard]] inline bool verify_receipt(const LockboxReceipt &r) noexcept {
  return lockbox_receipt_hash(r) == r.receipt_hash;
}

// ===========================================================================
//  LockboxAuditSink — where opens are recorded. Implementations must make
//  append() the commit point: after a successful append, is_opened() is true
//  for that content address.
// ===========================================================================
class LockboxAuditSink {
public:
  virtual ~LockboxAuditSink() = default;
  [[nodiscard]] virtual bool is_opened(atx::u64 content_address) const = 0;
  [[nodiscard]] virtual atx::u64 next_sequence() const = 0;
  [[nodiscard]] virtual atx::u64 last_receipt_hash() const = 0;
  [[nodiscard]] virtual atx::core::Status append(const LockboxReceipt &receipt) = 0;
};

// Process-local sink (tests, single-run research engines).
class InMemoryLockboxAudit final : public LockboxAuditSink {
public:
  [[nodiscard]] bool is_opened(atx::u64 content_address) const override {
    return opened_.find(content_address) != opened_.end();
  }
  [[nodiscard]] atx::u64 next_sequence() const override {
    return static_cast<atx::u64>(receipts_.size());
  }
  [[nodiscard]] atx::u64 last_receipt_hash() const override {
    return receipts_.empty() ? 0U : receipts_.back().receipt_hash;
  }
  [[nodiscard]] atx::core::Status append(const LockboxReceipt &receipt) override {
    opened_.insert(receipt.content_address);
    receipts_.push_back(receipt);
    return atx::core::Ok();
  }
  [[nodiscard]] const std::vector<LockboxReceipt> &receipts() const noexcept { return receipts_; }

private:
  std::unordered_set<atx::u64> opened_;
  std::vector<LockboxReceipt> receipts_;
};

// Durable, append-only, one tab-separated line per receipt:
//   ATXLBX1 seq addr cand begin end prev hash purpose requester   (hex numbers)
// open() replays and VERIFIES the whole chain (hash + prev link + sequence);
// any mismatch is Err(ParseError) — the log was edited. A final line without
// its newline is a torn append (crash) and is ignored and truncated.
class FileLockboxAudit final : public LockboxAuditSink {
public:
  [[nodiscard]] static atx::core::Result<FileLockboxAudit>
  open(const std::filesystem::path &path) {
    using atx::core::Err;
    using atx::core::ErrorCode;
    FileLockboxAudit out;
    out.path_ = path;
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
      std::ifstream in(path, std::ios::binary);
      if (!in) {
        return Err(ErrorCode::IoError, "FileLockboxAudit: cannot read " + path.string());
      }
      const std::string text((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
      in.close();
      atx::usize pos = 0;
      // Bounded: each iteration consumes one complete line.
      while (pos < text.size()) {
        const atx::usize nl = text.find('\n', pos);
        if (nl == std::string::npos) {
          break; // torn tail
        }
        auto rec = parse_line(std::string_view(text).substr(pos, nl - pos));
        if (!rec.has_value() || !verify_receipt(*rec) ||
            rec->sequence != out.receipts_.size() ||
            rec->prev_receipt_hash != out.last_receipt_hash()) {
          return Err(ErrorCode::ParseError,
                     "FileLockboxAudit: audit chain verification failed (log edited?)");
        }
        out.opened_.insert(rec->content_address);
        out.receipts_.push_back(std::move(*rec));
        pos = nl + 1U;
      }
      if (pos != text.size()) {
        std::filesystem::resize_file(path, pos, ec);
        if (ec) {
          return Err(ErrorCode::IoError, "FileLockboxAudit: cannot repair torn tail");
        }
      }
    }
    return atx::core::Ok(std::move(out));
  }

  [[nodiscard]] bool is_opened(atx::u64 content_address) const override {
    return opened_.find(content_address) != opened_.end();
  }
  [[nodiscard]] atx::u64 next_sequence() const override {
    return static_cast<atx::u64>(receipts_.size());
  }
  [[nodiscard]] atx::u64 last_receipt_hash() const override {
    return receipts_.empty() ? 0U : receipts_.back().receipt_hash;
  }
  [[nodiscard]] atx::core::Status append(const LockboxReceipt &r) override {
    std::string line = "ATXLBX1";
    for (const atx::u64 v : {r.sequence, r.content_address, r.candidate_hash,
                             static_cast<atx::u64>(r.holdout_begin),
                             static_cast<atx::u64>(r.holdout_end), r.prev_receipt_hash,
                             r.receipt_hash}) {
      line += '\t';
      line += to_hex(v);
    }
    line += '\t';
    line += detail::sanitize_audit_text(r.purpose);
    line += '\t';
    line += detail::sanitize_audit_text(r.requester);
    line += '\n';
    std::ofstream out(path_, std::ios::binary | std::ios::app);
    out.write(line.data(), static_cast<std::streamsize>(line.size()));
    out.flush();
    if (!out) {
      return atx::core::Err(atx::core::ErrorCode::IoError,
                            "FileLockboxAudit: durable append failed");
    }
    opened_.insert(r.content_address);
    receipts_.push_back(r);
    return atx::core::Ok();
  }
  [[nodiscard]] const std::vector<LockboxReceipt> &receipts() const noexcept { return receipts_; }

private:
  FileLockboxAudit() = default;

  [[nodiscard]] static std::string to_hex(atx::u64 v) {
    char buf[17] = {};
    const auto res = std::to_chars(buf, buf + 16, v, 16);
    return std::string(buf, res.ptr);
  }

  [[nodiscard]] static std::optional<LockboxReceipt> parse_line(std::string_view line) {
    std::vector<std::string_view> f;
    atx::usize p = 0;
    // Bounded: a line has at most line.size() + 1 tab-separated fields.
    for (atx::usize guard = 0; guard <= line.size(); ++guard) {
      const atx::usize tab = line.find('\t', p);
      f.push_back(line.substr(p, tab == std::string_view::npos ? line.size() - p : tab - p));
      if (tab == std::string_view::npos) {
        break;
      }
      p = tab + 1U;
    }
    if (f.size() != 10U || f[0] != "ATXLBX1") {
      return std::nullopt;
    }
    atx::u64 v[7] = {};
    for (atx::usize i = 0; i < 7U; ++i) {
      const std::string_view s = f[i + 1U];
      const auto res = std::from_chars(s.data(), s.data() + s.size(), v[i], 16);
      if (res.ec != std::errc{} || res.ptr != s.data() + s.size() || s.empty()) {
        return std::nullopt;
      }
    }
    LockboxReceipt r;
    r.sequence = v[0];
    r.content_address = v[1];
    r.candidate_hash = v[2];
    r.holdout_begin = static_cast<atx::usize>(v[3]);
    r.holdout_end = static_cast<atx::usize>(v[4]);
    r.prev_receipt_hash = v[5];
    r.receipt_hash = v[6];
    r.purpose = std::string(f[8]);
    r.requester = std::string(f[9]);
    return r;
  }

  std::filesystem::path path_;
  std::unordered_set<atx::u64> opened_;
  std::vector<LockboxReceipt> receipts_;
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
  ATX_TRY(alpha::Panel holdout, detail::slice_panel(full, begin, end));

  LockboxReceipt r;
  r.sequence = sink.next_sequence();
  r.content_address = res.content_address;
  r.candidate_hash = req.candidate_hash;
  r.holdout_begin = begin;
  r.holdout_end = end;
  r.purpose = detail::sanitize_audit_text(req.purpose);
  r.requester = detail::sanitize_audit_text(req.requester);
  r.prev_receipt_hash = sink.last_receipt_hash();
  r.receipt_hash = lockbox_receipt_hash(r);
  ATX_TRY_VOID(sink.append(r));
  return atx::core::Ok(LockboxOpening{std::move(holdout), std::move(r)});
}

} // namespace atx::engine::eval
