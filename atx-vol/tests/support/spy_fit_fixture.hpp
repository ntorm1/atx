#pragma once

// Ten real Databento OPRA SPY slices selected to exercise cold fitting across
// intraday liquidity and volatility regimes. The Parquet payloads live outside
// git under data/spy_fit_slices (C:/atx/data is the shared developer cache), so
// source-only CI can skip while a fixture-enabled run gets a stable matrix.

#include <array>
#include <filesystem>
#include <optional>
#include <string>

#include "opra_fixture.hpp"

namespace atx::vol::testkit {

struct SpyFitFixture {
  const char *id;
  const char *filename;
  const char *snapshot_iso;
  const char *regime;
};

inline constexpr std::array<SpyFitFixture, 10> kSpyFitFixtures{{
    // Entry 0 regenerated from the on-disk raw OPRA CBBO pull
    // C:/atx-data/spy-dispersion/opra/SPY/2026-01-02.parquet (the processed
    // 2026-02-12 slice is gone). The snapshot stamp MUST match the raw data's
    // real minute (2026-01-02T19:55Z) — it is NOT cosmetic: the loader uses it
    // to compute every year-fraction and to drop 0DTE/expired expiries, so a
    // mismatched stamp would silently discard ~14 near expiries. Regenerate via
    //   python atx-vol/tools/make_fit_slice.py \
    //     --src C:/atx-data/spy-dispersion/opra/SPY/2026-01-02.parquet \
    //     --out data/spy_fit_slices/SPY_2026-01-02T1955Z.parquet --underlying SPY
    // Yields 9626 quotes over 32 expiries (the E1 fit_workers wall-win slice).
    {"jan02-close", "SPY_2026-01-02T1955Z.parquet", "2026-01-02T19:55:00Z", "regen/close"},
    {"selloff-mid", "SPY_2026-02-12T1700Z.parquet", "2026-02-12T17:00:00Z", "selloff/midday"},
    {"selloff-pm", "SPY_2026-02-12T1955Z.parquet", "2026-02-12T19:55:00Z", "selloff/afternoon"},
    {"rally-open", "SPY_2026-03-09T1335Z.parquet", "2026-03-09T13:35:00Z", "wide-rally/open"},
    {"rally-mid", "SPY_2026-03-09T1600Z.parquet", "2026-03-09T16:00:00Z", "wide-rally/midday"},
    {"rally-close", "SPY_2026-03-09T1955Z.parquet", "2026-03-09T19:55:00Z", "wide-rally/preclose"},
    {"calm-open", "SPY_2026-05-27T1335Z.parquet", "2026-05-27T13:35:00Z", "calm/open"},
    {"calm-mid", "SPY_2026-05-27T1600Z.parquet", "2026-05-27T16:00:00Z", "calm/midday"},
    {"calm-close", "SPY_2026-05-27T1955Z.parquet", "2026-05-27T19:55:00Z", "calm/preclose"},
    {"stress-close", "SPY_2026-06-05T1955Z.parquet", "2026-06-05T19:55:00Z",
     "high-vol-selloff/preclose"},
}};

[[nodiscard]] inline std::string find_spy_fit_parquet(const SpyFitFixture &fixture) {
  const char *dirs[] = {"data/spy_fit_slices/", "../data/spy_fit_slices/",
                        "../../data/spy_fit_slices/", "../../../data/spy_fit_slices/",
                        "C:/atx/data/spy_fit_slices/"};
  for (const char *dir : dirs) {
    const std::string path = std::string(dir) + fixture.filename;
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
      return path;
    }
  }
  return {};
}

[[nodiscard]] inline std::optional<OpraBoard> load_spy_fit_fixture(const SpyFitFixture &fixture,
                                                                   double r = 0.043) {
  return load_opra_board_path(find_spy_fit_parquet(fixture), "SPY", fixture.snapshot_iso, r);
}

} // namespace atx::vol::testkit
