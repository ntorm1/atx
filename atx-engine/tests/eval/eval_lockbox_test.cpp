// atx::engine::eval — lockbox.hpp tests (S4.4b, suite EvalLockbox).
//
// Proves the point-in-time LOCKBOX RESERVATION + SEAL that S4.5's mine->gate->
// admit->combine pipeline runs UPSTREAM OF, and that S8.2 opens exactly once.
// reserve_lockbox carves the terminal contiguous most-recent `frac` of dates as
// the lockbox, inserts a CPCV-width embargo gap immediately before it, and hands
// back a SealedPanel that exposes ONLY the visible region [0, lockbox_begin -
// embargo_len). Any read into the sealed region [lockbox_begin, T) traps.
//
// The load-bearing proofs:
//   * UpstreamStagesRunCleanOnVisible — a fitness-style cross-sectional eval AND
//     a combine-style aggregation over SealedPanel::visible() run clean (every
//     date they touch is < visible_len; the visible Panel is a real Panel).
//   * SealedReadTraps — a direct read of the sealed region fails: the Result form
//     returns Err, and the ATX_ASSERT form aborts (death test).
//   * IdentityRoundTrips — the same reservation on the same panel reproduces
//     byte-identically (same boundaries, same content-address). NO RNG.
//   * Edge cases — embargo+frac must leave a non-empty visible region (Err if
//     not); off-by-one boundary arithmetic at lockbox_begin / lockbox_begin -
//     embargo_len.

#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/eval/cpcv.hpp"
#include "atx/engine/eval/lockbox.hpp"

namespace {

using atx::f64;
using atx::u8;
using atx::usize;
using atx::core::ErrorCode;
using atx::engine::alpha::FieldId;
using atx::engine::alpha::Panel;
using atx::engine::eval::CpcvConfig;
using atx::engine::eval::reserve_lockbox;
using atx::engine::eval::SealedPanel;
using atx::engine::eval::SealedReservation;

// A tiny deterministic LCG -> uniform(-1, 1), the S3/S4 fixture idiom (no RNG dep).
struct Lcg {
  std::uint64_t s;
  [[nodiscard]] f64 next() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t hi = s >> 11U;
    const f64 u = static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U);
    return 2.0 * u - 1.0;
  }
};

// Build a deterministic multi-instrument close+volume panel of `dates` rows.
[[nodiscard]] Panel make_panel(usize dates, usize insts, std::uint64_t seed) {
  std::vector<f64> close(dates * insts);
  std::vector<f64> volume(dates * insts);
  Lcg rng{seed};
  for (usize i = 0; i < insts; ++i) {
    f64 px = 100.0 + static_cast<f64>(i);
    for (usize t = 0; t < dates; ++t) {
      px *= (1.0 + 0.01 * rng.next());
      close[t * insts + i] = px;
      volume[t * insts + i] = 1000.0 + 10.0 * rng.next();
    }
  }
  auto r = Panel::create(dates, insts, {"close", "volume"}, {close, volume}, {});
  EXPECT_TRUE(r.has_value()) << "fixture panel must build";
  return std::move(r.value());
}

// =============================================================================
//  BoundaryArithmetic — frac=0.20 over T=100 carves the terminal 20 dates as the
//  lockbox (lockbox_begin == 80); a CPCV embargo of 0.01*100 == 1 date sits just
//  before it, so visible_len == 79. Off-by-one checks at every boundary.
// =============================================================================
TEST(EvalLockbox, BoundaryArithmetic) {
  const Panel panel = make_panel(/*dates*/ 100, /*insts*/ 4, 0xABCDu);
  const usize embargo_len = 1; // CpcvConfig{embargo=0.01} over T=100 -> ceil(1.0)=1
  auto r = reserve_lockbox(panel, /*frac*/ 0.20, embargo_len);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  const SealedPanel &sp = r.value();
  const SealedReservation res = sp.reservation();

  EXPECT_EQ(res.dates, 100u);
  EXPECT_EQ(res.instruments, 4u);
  EXPECT_EQ(res.lockbox_begin, 80u); // T - floor(0.20*T) == 100 - 20
  EXPECT_EQ(res.embargo_len, embargo_len);
  EXPECT_EQ(res.visible_len, 79u); // [0, lockbox_begin - embargo_len) == [0, 79)
  EXPECT_EQ(sp.visible().dates(), 79u);
  EXPECT_EQ(sp.visible().instruments(), 4u);
}

// =============================================================================
//  EmbargoFromCpcvConfig — the embargo width derives from the CPCV embargo
//  fraction x T (cpcv.hpp embargo_len == ceil(h * T)). h=0.05 over T=80 -> 4.
// =============================================================================
TEST(EvalLockbox, EmbargoFromCpcvConfig) {
  const Panel panel = make_panel(/*dates*/ 80, /*insts*/ 3, 0x55u);
  CpcvConfig cfg;
  cfg.embargo = 0.05; // ceil(0.05 * 80) == 4
  auto r = reserve_lockbox(panel, /*frac*/ 0.25, cfg);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  const SealedReservation res = r.value().reservation();
  EXPECT_EQ(res.lockbox_begin, 60u); // 80 - floor(0.25*80) == 80 - 20
  EXPECT_EQ(res.embargo_len, 4u);
  EXPECT_EQ(res.visible_len, 56u); // 60 - 4
}

// =============================================================================
//  UpstreamStagesRunCleanOnVisible — a fitness-style cross-sectional eval AND a
//  combine-style aggregation over the SealedPanel's visible() Panel run clean:
//  every date they read is < visible_len, and visible() is a real Panel they can
//  consume with the ordinary accessor surface (the mine/gate/combine seam).
// =============================================================================
TEST(EvalLockbox, UpstreamStagesRunCleanOnVisible) {
  const Panel panel = make_panel(/*dates*/ 120, /*insts*/ 5, 0x99u);
  auto r = reserve_lockbox(panel, /*frac*/ 0.20, /*embargo_len*/ 2);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  const SealedPanel &sp = r.value();
  const Panel &vis = sp.visible();
  ASSERT_LT(vis.dates(), panel.dates()); // strictly truncated

  const FieldId close = vis.field_id("close").value();

  // Fitness-style stage: per-date cross-sectional mean return of the close field
  // over the WHOLE visible region. Reads only dates in [0, visible_len) -> clean.
  f64 fitness_acc = 0.0;
  usize fitness_n = 0;
  for (usize t = 1; t < vis.dates(); ++t) {
    const std::span<const f64> prev = vis.field_cross_section(close, t - 1U);
    const std::span<const f64> cur = vis.field_cross_section(close, t);
    f64 row = 0.0;
    for (usize j = 0; j < vis.instruments(); ++j) {
      row += cur[j] / prev[j] - 1.0;
    }
    fitness_acc += row / static_cast<f64>(vis.instruments());
    ++fitness_n;
  }
  EXPECT_EQ(fitness_n, vis.dates() - 1U);
  EXPECT_TRUE(std::isfinite(fitness_acc));

  // Combine-style stage: aggregate the per-date fitness into a single book score
  // (equal-weight combine over the visible window). Also clean (visible reads).
  const f64 book = fitness_acc / static_cast<f64>(fitness_n);
  EXPECT_TRUE(std::isfinite(book));

  // The guarded accessor returns Ok for every visible date.
  for (usize t = 0; t < vis.dates(); ++t) {
    EXPECT_TRUE(sp.field_cross_section(close, t).has_value());
  }
}

// =============================================================================
//  SealedReadTraps (Result form) — a direct read of the sealed region returns
//  Err. lockbox_begin and lockbox_begin - 1 (the embargo gap) are both sealed.
// =============================================================================
TEST(EvalLockbox, SealedReadTrapsResultForm) {
  const Panel panel = make_panel(/*dates*/ 100, /*insts*/ 4, 0x1u);
  auto r = reserve_lockbox(panel, /*frac*/ 0.20, /*embargo_len*/ 3);
  ASSERT_TRUE(r.has_value());
  const SealedPanel &sp = r.value();
  const SealedReservation res = sp.reservation();
  const FieldId close = sp.visible().field_id("close").value();

  // Last visible date is OK; the first embargo date is sealed.
  EXPECT_TRUE(sp.field_cross_section(close, res.visible_len - 1U).has_value());
  const auto embargo_read = sp.field_cross_section(close, res.visible_len);
  ASSERT_FALSE(embargo_read.has_value());
  EXPECT_EQ(embargo_read.error().code(), ErrorCode::PermissionDenied);

  // lockbox_begin and the final date are sealed.
  EXPECT_FALSE(sp.field_cross_section(close, res.lockbox_begin).has_value());
  EXPECT_FALSE(sp.field_cross_section(close, res.dates - 1U).has_value());
  // Out-of-range date is OutOfRange, distinct from a sealed PermissionDenied.
  EXPECT_EQ(sp.field_cross_section(close, res.dates).error().code(), ErrorCode::OutOfRange);
}

// =============================================================================
//  SealedReadTraps (ATX_ASSERT form) — the trapping accessor aborts on a sealed
//  read (the PIT seal nothing upstream may cross). Death test.
// =============================================================================
TEST(EvalLockboxDeathTest, SealedReadTrapsAssertForm) {
  const Panel panel = make_panel(/*dates*/ 100, /*insts*/ 4, 0x2u);
  auto r = reserve_lockbox(panel, /*frac*/ 0.20, /*embargo_len*/ 3);
  ASSERT_TRUE(r.has_value());
  const SealedPanel &sp = r.value();
  const SealedReservation res = sp.reservation();
  const FieldId close = sp.visible().field_id("close").value();
  // Reading at lockbox_begin via the trapping accessor must abort.
  EXPECT_DEATH((void)sp.field_cross_section_or_trap(close, res.lockbox_begin), ".*");
}

// =============================================================================
//  IdentityRoundTrips — the same reservation on the same panel reproduces byte-
//  identically: same boundaries AND same content-address (a deterministic
//  function of the panel, NO RNG). A different frac/embargo shifts the address.
// =============================================================================
TEST(EvalLockbox, IdentityRoundTrips) {
  const Panel panel = make_panel(/*dates*/ 90, /*insts*/ 3, 0xF00Du);
  auto a = reserve_lockbox(panel, /*frac*/ 0.20, /*embargo_len*/ 2);
  auto b = reserve_lockbox(panel, /*frac*/ 0.20, /*embargo_len*/ 2);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  const SealedReservation ra = a.value().reservation();
  const SealedReservation rb = b.value().reservation();
  EXPECT_EQ(ra.lockbox_begin, rb.lockbox_begin);
  EXPECT_EQ(ra.embargo_len, rb.embargo_len);
  EXPECT_EQ(ra.visible_len, rb.visible_len);
  EXPECT_EQ(ra.content_address, rb.content_address);
  EXPECT_NE(ra.content_address, 0u) << "content-address must be a real digest";

  // A different reservation geometry over the same panel shifts the address.
  auto c = reserve_lockbox(panel, /*frac*/ 0.30, /*embargo_len*/ 2);
  ASSERT_TRUE(c.has_value());
  EXPECT_NE(c.value().reservation().content_address, ra.content_address);

  // A different panel shifts the address even at the same geometry.
  const Panel other = make_panel(/*dates*/ 90, /*insts*/ 3, 0xBEEFu);
  auto d = reserve_lockbox(other, /*frac*/ 0.20, /*embargo_len*/ 2);
  ASSERT_TRUE(d.has_value());
  EXPECT_NE(d.value().reservation().content_address, ra.content_address);
}

// =============================================================================
//  EmptyVisibleRegionIsError — embargo + frac that consume the whole panel (no
//  non-empty visible region) is an InvalidArgument. Boundary: visible_len == 0.
// =============================================================================
TEST(EvalLockbox, EmptyVisibleRegionIsError) {
  const Panel panel = make_panel(/*dates*/ 20, /*insts*/ 2, 0x7u);
  // frac=0.50 -> lockbox_begin=10; embargo_len=10 -> visible_len would be 0.
  auto r = reserve_lockbox(panel, /*frac*/ 0.50, /*embargo_len*/ 10);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);

  // embargo_len exceeding lockbox_begin (would underflow) is also rejected.
  auto r2 = reserve_lockbox(panel, /*frac*/ 0.50, /*embargo_len*/ 11);
  ASSERT_FALSE(r2.has_value());
  EXPECT_EQ(r2.error().code(), ErrorCode::InvalidArgument);
}

// =============================================================================
//  InvalidFracIsError — frac must lie in (0, 1): a lockbox of 0 dates (frac too
//  small to carve a single date) OR a lockbox of every date is rejected.
// =============================================================================
TEST(EvalLockbox, InvalidFracIsError) {
  const Panel panel = make_panel(/*dates*/ 50, /*insts*/ 2, 0x8u);
  // frac too small to carve even one date (floor(0.01*50) == 0 -> empty lockbox).
  EXPECT_FALSE(reserve_lockbox(panel, /*frac*/ 0.01, /*embargo_len*/ 0).has_value());
  // frac >= 1.0 carves the whole panel (no visible region).
  EXPECT_FALSE(reserve_lockbox(panel, /*frac*/ 1.0, /*embargo_len*/ 0).has_value());
  // frac <= 0 is invalid.
  EXPECT_FALSE(reserve_lockbox(panel, /*frac*/ 0.0, /*embargo_len*/ 0).has_value());
  // A valid frac that carves >= 1 lockbox date and leaves a visible region is Ok.
  EXPECT_TRUE(reserve_lockbox(panel, /*frac*/ 0.20, /*embargo_len*/ 0).has_value());
}

} // namespace
