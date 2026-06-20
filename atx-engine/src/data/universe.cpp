// atx::engine::data — universe construction implementation (p3 S1-4).
//
// See universe.hpp for the full contract + the split-invariance / causal-ADV /
// sector-fallback / survivorship caveats. The flow is:
//   axis-validate(Panel, Dataset)
//     -> market_cap  = shares (corp col) × raw close (Panel)
//     -> adv_usd     = Panel adv{window} if present, else causal roll of dollar_volume
//     -> sector_code = GICS (corp col) else SIC (corp col) else sentinel
//     -> in_universe = present ∧ mktcap-floor ∧ adv-floor ∧ top-N-by-ADV
//
// Every step is COLD-PATH (once per backtest window); the std::vector allocation
// is intentional. Helpers keep each function short and each invariant legible.

#include "atx/engine/data/universe.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/alpha/datafields.hpp" // kClose, kVolume, kDollarVolume, kAdvPrefix
#include "atx/engine/alpha/panel.hpp"

namespace atx::engine::data {

namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;

namespace df = atx::engine::alpha::datafields;

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

// Canonical corp-action column indices (mirrors corporate_actions.hpp's load-
// bearing column order — read positionally, validated by the 6-column check).
constexpr atx::usize kCorpColShares = 2; // shares_outstanding (PIT fwd-filled; NaN if absent)
constexpr atx::usize kCorpColGics = 4;   // gics_sector_code (kNoSector f64 sentinel if absent)
constexpr atx::usize kCorpColSic = 5;    // sic_code (kNoSector f64 sentinel if absent)

// Resolve a Panel field's whole-column span by name, or Err(InvalidArgument).
[[nodiscard]] Result<std::span<const atx::f64>> panel_field(const alpha::Panel &p,
                                                            std::string_view name) {
  const auto id = p.field_id(name);
  if (!id.has_value()) {
    return Err(ErrorCode::InvalidArgument,
               std::string{"build_universe: missing required Panel field '"} + std::string{name} +
                   "'");
  }
  return Ok(p.field_all(id.value()));
}

// Causal trailing mean of `src` over a `d`-bar window, matching the engine's
// ts_mean / datafields policy EXACTLY: per instrument column, value at date t
// requires a FULL window [t-d+1, t] with NO NaN, else NaN; the mean sums
// oldest→newest / d. Identical to alpha::datafields::detail::rolling_mean (kept
// local rather than reaching into another header's detail namespace, so the
// causal/no-look-ahead policy is owned + asserted here too).
[[nodiscard]] std::vector<atx::f64> causal_rolling_mean(std::span<const atx::f64> src,
                                                        atx::usize dates, atx::usize instruments,
                                                        atx::usize d) {
  std::vector<atx::f64> out(dates * instruments, kNaN);
  if (d == 0) {
    return out;
  }
  for (atx::usize j = 0; j < instruments; ++j) {
    for (atx::usize t = 0; t < dates; ++t) {
      if (t + 1 < d) {
        continue; // incomplete trailing window -> NaN (no look-ahead borrow)
      }
      atx::f64 sum = 0.0;
      bool ok = true;
      for (atx::usize k = t + 1 - d; k <= t; ++k) { // oldest -> newest, dates ≤ t only
        const atx::f64 v = src[k * instruments + j];
        if (std::isnan(v)) {
          ok = false;
          break;
        }
        sum += v;
      }
      if (ok) {
        out[t * instruments + j] = sum / static_cast<atx::f64>(d);
      }
    }
  }
  return out;
}

// Market cap = shares_outstanding (corp, PIT as-of) × raw close (Panel). RAW —
// NOT adjusted — so the product is split-invariant. NaN if either input is NaN.
[[nodiscard]] std::vector<atx::f64> market_cap_field(std::span<const atx::f64> raw_close,
                                                     std::span<const atx::f64> shares,
                                                     atx::usize cells) {
  std::vector<atx::f64> cap(cells, kNaN);
  for (atx::usize i = 0; i < cells; ++i) {
    cap[i] = shares[i] * raw_close[i]; // NaN propagates from either factor
  }
  return cap;
}

// ADV (dollars/day): prefer the Panel's pre-computed `adv{window}` column (the
// canonical causal datafield); else roll `dollar_volume` if the Panel carries
// it; else derive dollar_volume = close × volume and roll that. All three paths
// yield the SAME causal trailing mean — the Panel column is consumed verbatim,
// never recomputed differently.
[[nodiscard]] std::vector<atx::f64> adv_field(const alpha::Panel &p,
                                              std::span<const atx::f64> raw_close,
                                              std::span<const atx::f64> volume, atx::usize dates,
                                              atx::usize instruments, atx::usize window) {
  const std::string adv_name = std::string{df::kAdvPrefix} + std::to_string(window);
  if (const auto id = p.field_id(adv_name); id.has_value()) {
    const std::span<const atx::f64> col = p.field_all(id.value());
    return std::vector<atx::f64>(col.begin(), col.end()); // consume the panel datafield verbatim
  }
  const atx::usize cells = dates * instruments;
  if (const auto id = p.field_id(df::kDollarVolume); id.has_value()) {
    return causal_rolling_mean(p.field_all(id.value()), dates, instruments, window);
  }
  std::vector<atx::f64> dvol(cells, kNaN);
  for (atx::usize i = 0; i < cells; ++i) {
    dvol[i] = raw_close[i] * volume[i]; // NaN inputs propagate
  }
  return causal_rolling_mean(dvol, dates, instruments, window);
}

// Sector = GICS code if present at (t,i), else SIC fallback, else kNoSectorCode.
// The corp Dataset stores codes f64-widened with kNoSector (-1.0) as the f64
// "absent" sentinel; a present code is a non-negative integer. Missing → -1,
// NEVER 0 (0 is a valid-looking sector and would silently misclassify).
[[nodiscard]] atx::i32 resolve_sector(atx::f64 gics, atx::f64 sic) noexcept {
  if (gics != kNoSector && !std::isnan(gics)) {
    return static_cast<atx::i32>(gics);
  }
  if (sic != kNoSector && !std::isnan(sic)) {
    return static_cast<atx::i32>(sic);
  }
  return kNoSectorCode;
}

[[nodiscard]] std::vector<atx::i32> sector_field(std::span<const atx::f64> gics,
                                                 std::span<const atx::f64> sic, atx::usize cells) {
  std::vector<atx::i32> out(cells, kNoSectorCode);
  for (atx::usize i = 0; i < cells; ++i) {
    out[i] = resolve_sector(gics[i], sic[i]);
  }
  return out;
}

// Apply the top-N-by-ADV count cap to one date's already-floor-passed mask. Ranks
// the passing instruments by ADV DESCENDING; ties break by ascending instrument
// id (the canonical column index — deterministic). Keeps the first `top_n`, drops
// the rest from the mask. A non-positive `top_n` is the no-cap case (caller skips
// this entirely). NaN ADV never passes the floor, so it never reaches here.
void apply_top_n(std::span<atx::u8> mask_row, std::span<const atx::f64> adv_row, atx::usize top_n) {
  std::vector<atx::usize> passing;
  passing.reserve(adv_row.size());
  for (atx::usize i = 0; i < mask_row.size(); ++i) {
    if (mask_row[i] != 0) {
      passing.push_back(i);
    }
  }
  if (passing.size() <= top_n) {
    return; // already within the cap
  }
  // Descending by ADV; canonical-id ascending tie-break (deterministic).
  std::stable_sort(passing.begin(), passing.end(), [&](atx::usize a, atx::usize b) {
    if (adv_row[a] != adv_row[b]) {
      return adv_row[a] > adv_row[b];
    }
    return a < b;
  });
  for (atx::usize rank = top_n; rank < passing.size(); ++rank) {
    mask_row[passing[rank]] = 0; // beyond the cap -> dropped
  }
}

// Build the PIT membership mask: present(t,i) ∧ mktcap-floor ∧ adv-floor, then the
// optional top-N-by-ADV cap per date. A NaN market_cap or adv_usd FAILS its floor
// (NaN compares false), so a NaN price/shares cell is excluded — the no-
// survivorship / no-look-ahead guard.
[[nodiscard]] std::vector<atx::u8> membership_mask(const alpha::Panel &p,
                                                   std::span<const atx::f64> market_cap,
                                                   std::span<const atx::f64> adv_usd,
                                                   std::span<const atx::f64> raw_close,
                                                   std::span<const atx::i32> sector_code,
                                                   const UniverseConfig &cfg, atx::usize dates,
                                                   atx::usize instruments) {
  std::vector<atx::u8> mask(dates * instruments, atx::u8{0});
  for (atx::usize t = 0; t < dates; ++t) {
    for (atx::usize i = 0; i < instruments; ++i) {
      const atx::usize flat = t * instruments + i;
      const bool present = p.in_universe(t, i);
      const bool cap_ok = market_cap[flat] >= cfg.min_mktcap_usd; // NaN -> false
      const bool adv_ok = adv_usd[flat] >= cfg.min_adv_usd;       // NaN -> false
      // Price floor: STRICT raw_close > min_price (NaN -> false). Disabled (no
      // change) when min_price <= 0 so legacy panels are byte-identical.
      const bool price_ok = (cfg.min_price <= 0.0) || (raw_close[flat] > cfg.min_price);
      // Single-stock requirement: a classified (non-sentinel) sector. ETFs/funds
      // carry no GICS, so sector_code == kNoSectorCode excludes them. Off by default.
      const bool sector_ok = !cfg.require_sector || (sector_code[flat] != kNoSectorCode);
      mask[flat] =
          (present && cap_ok && adv_ok && price_ok && sector_ok) ? atx::u8{1} : atx::u8{0};
    }
    if (cfg.top_n_by_adv > 0) {
      apply_top_n(std::span<atx::u8>{mask}.subspan(t * instruments, instruments),
                  adv_usd.subspan(t * instruments, instruments), cfg.top_n_by_adv);
    }
  }
  return mask;
}

// Validate the price Panel and corp-action Dataset share the canonical axis (same
// date + instrument counts) and that the Dataset is the 6-column corp-action
// shape. Order/identity alignment is S1-5's contract (price defines the axis); we
// enforce SHAPE here so a positional read can't silently misindex.
[[nodiscard]] Result<void> validate_axes(const alpha::Panel &p, const Dataset &corp) {
  if (corp.schema().columns.size() != kCorpActionColumnCount) {
    return Err(ErrorCode::InvalidArgument,
               "build_universe: corp_actions is not a 6-column corporate-action Dataset");
  }
  if (corp.num_dates() != p.dates() || corp.num_instruments() != p.instruments()) {
    return Err(ErrorCode::InvalidArgument,
               "build_universe: corp_actions axis does not match the price Panel axis "
               "(caller must align corp_actions onto the price-defined axis first)");
  }
  return Ok();
}

} // namespace

atx::core::Result<UniverseFields> build_universe(const alpha::Panel &price_panel,
                                                 const Dataset &corp_actions,
                                                 const UniverseConfig &cfg) {
  if (auto v = validate_axes(price_panel, corp_actions); !v.has_value()) {
    return Err(v.error());
  }

  const atx::usize dates = price_panel.dates();
  const atx::usize instruments = price_panel.instruments();
  const atx::usize cells = dates * instruments;

  ATX_TRY(const std::span<const atx::f64> raw_close, panel_field(price_panel, df::kClose));
  ATX_TRY(const std::span<const atx::f64> volume, panel_field(price_panel, df::kVolume));

  const std::span<const atx::f64> shares = corp_actions.column(kCorpColShares);
  const std::span<const atx::f64> gics = corp_actions.column(kCorpColGics);
  const std::span<const atx::f64> sic = corp_actions.column(kCorpColSic);

  UniverseFields out;
  out.market_cap = market_cap_field(raw_close, shares, cells);
  out.adv_usd = adv_field(price_panel, raw_close, volume, dates, instruments, cfg.adv_window);
  out.sector_code = sector_field(gics, sic, cells);
  out.in_universe = membership_mask(price_panel, out.market_cap, out.adv_usd, raw_close,
                                    out.sector_code, cfg, dates, instruments);
  return Ok(std::move(out));
}

} // namespace atx::engine::data
