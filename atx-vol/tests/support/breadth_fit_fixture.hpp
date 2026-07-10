#pragma once

// Real Databento OPRA breadth corpus. Payloads are generated with the
// repository's cost-gated cbbo-1m loader and live outside git in the shared data
// cache. Together with the ten-slice SPY matrix this covers dense ETFs, liquid
// single names, a sparse small-cap board, a volatility product, open/close, and
// before/after scheduled earnings announcements.

#include <array>
#include <filesystem>
#include <optional>
#include <string>

#include "atx/vol/fit_policy.hpp"
#include "opra_fixture.hpp"

namespace atx::vol::testkit {

struct BreadthFitFixture {
  const char *id;
  const char *symbol;
  const char *filename;
  const char *snapshot_iso;
  const char *regime;
  MarketSessionPhase session_phase;
  EventPhase event_phase;
  bool vol_product;
  VolCurveKind expected_curve;
  double min_clean_pct;
};

inline constexpr std::array<BreadthFitFixture, 14> kBreadthFitFixtures{{
    {"qqq-open", "QQQ", "QQQ_2026-06-05T1332Z.parquet", "2026-06-05T13:32:00Z", "dense-etf/open+2m",
     MarketSessionPhase::Opening, EventPhase::None, false, VolCurveKind::LinearVariance, 98.0},
    {"qqq-close", "QQQ", "QQQ_2026-06-05T1958Z.parquet", "2026-06-05T19:58:00Z",
     "dense-etf/close-2m", MarketSessionPhase::Closing, EventPhase::None, false,
     VolCurveKind::LinearVariance, 98.0},
    {"iwm-open", "IWM", "IWM_2026-06-05T1332Z.parquet", "2026-06-05T13:32:00Z",
     "smallcap-etf/open+2m", MarketSessionPhase::Opening, EventPhase::None, false,
     VolCurveKind::LinearVariance, 97.0},
    {"iwm-close", "IWM", "IWM_2026-06-05T1958Z.parquet", "2026-06-05T19:58:00Z",
     "smallcap-etf/close-2m", MarketSessionPhase::Closing, EventPhase::None, false,
     VolCurveKind::LinearVariance, 97.0},
    {"xom-open", "XOM", "XOM_2026-06-05T1332Z_2m.parquet", "2026-06-05T13:33:00Z",
     "liquid-single/open+2m", MarketSessionPhase::Opening, EventPhase::None, false,
     VolCurveKind::Essvi, 90.0},
    {"xom-close", "XOM", "XOM_2026-06-05T1958Z.parquet", "2026-06-05T19:58:00Z",
     "liquid-single/close-2m", MarketSessionPhase::Closing, EventPhase::None, false,
     VolCurveKind::Essvi, 90.0},
    {"soun-open", "SOUN", "SOUN_2026-06-05T1332Z.parquet", "2026-06-05T13:32:00Z",
     "sparse-smallcap/open+2m", MarketSessionPhase::Opening, EventPhase::None, false,
     VolCurveKind::Svi, 80.0},
    {"soun-close", "SOUN", "SOUN_2026-06-05T1958Z.parquet", "2026-06-05T19:58:00Z",
     "sparse-smallcap/close-2m", MarketSessionPhase::Closing, EventPhase::None, false,
     VolCurveKind::Svi, 80.0},
    {"vxx-open", "VXX", "VXX_2026-06-05T1332Z.parquet", "2026-06-05T13:32:00Z",
     "vol-product/open+2m", MarketSessionPhase::Opening, EventPhase::None, true, VolCurveKind::Svi,
     85.0},
    {"vxx-close", "VXX", "VXX_2026-06-05T1958Z.parquet", "2026-06-05T19:58:00Z",
     "vol-product/close-2m", MarketSessionPhase::Closing, EventPhase::None, true, VolCurveKind::Svi,
     85.0},
    {"aapl-pre", "AAPL", "AAPL_2026-04-30T1958Z.parquet", "2026-04-30T19:58:00Z",
     "earnings/pre-announcement", MarketSessionPhase::Closing, EventPhase::PreAnnouncement, false,
     VolCurveKind::LinearVariance, 98.0},
    {"aapl-post", "AAPL", "AAPL_2026-05-01T1332Z.parquet", "2026-05-01T13:32:00Z",
     "earnings/post-announcement", MarketSessionPhase::Opening, EventPhase::PostAnnouncement, false,
     VolCurveKind::LinearVariance, 98.0},
    {"amzn-pre", "AMZN", "AMZN_2026-04-29T1958Z.parquet", "2026-04-29T19:58:00Z",
     "earnings/pre-announcement", MarketSessionPhase::Closing, EventPhase::PreAnnouncement, false,
     VolCurveKind::LinearVariance, 98.0},
    {"amzn-post", "AMZN", "AMZN_2026-04-30T1332Z.parquet", "2026-04-30T13:32:00Z",
     "earnings/post-announcement", MarketSessionPhase::Opening, EventPhase::PostAnnouncement, false,
     VolCurveKind::LinearVariance, 98.0},
}};

[[nodiscard]] inline std::string find_breadth_fit_parquet(const BreadthFitFixture &fixture) {
  const char *dirs[] = {"data/vol_breadth_slices/", "../data/vol_breadth_slices/",
                        "../../data/vol_breadth_slices/", "../../../data/vol_breadth_slices/",
                        "C:/atx/data/vol_breadth_slices/"};
  for (const char *dir : dirs) {
    const std::string path = std::string(dir) + fixture.filename;
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
      return path;
    }
  }
  return {};
}

[[nodiscard]] inline std::optional<OpraBoard>
load_breadth_fit_fixture(const BreadthFitFixture &fixture, double r = 0.043) {
  return load_opra_board_path(find_breadth_fit_parquet(fixture), fixture.symbol,
                              fixture.snapshot_iso, r);
}

[[nodiscard]] inline FitContext breadth_fit_context(const BreadthFitFixture &fixture) {
  FitContext context;
  context.session_phase = fixture.session_phase;
  context.event_phase = fixture.event_phase;
  context.vol_product = fixture.vol_product;
  return context;
}

} // namespace atx::vol::testkit
