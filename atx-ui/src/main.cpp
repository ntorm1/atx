#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

#include "atx/ui/application.hpp"
#include "atx/ui/opra_vol_surface.hpp"
#include "atx/ui/vol_workspace.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

struct Options {
  atx::ui::OpraSourceConfig source;
  bool headless{false};
  bool help{false};
  std::size_t frame_limit{0};
};

void print_usage() {
  std::cout << "atx-ui [--symbol SYMBOL] [--data PATH] [--snapshot ISO] "
               "[--expiry YYYY-MM-DD] [--rate RATE] [--headless] [--frames N]\n";
}

bool parse_options(int argc, char **argv, Options &options) {
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string_view arg = argv[i];
      const auto next = [&]() -> std::string {
        if (i + 1 >= argc) {
          throw std::runtime_error("missing value after " + std::string(arg));
        }
        return argv[++i];
      };
      if (arg == "--symbol") {
        options.source.symbol = next();
      } else if (arg == "--data") {
        options.source.path = next();
      } else if (arg == "--snapshot") {
        options.source.snapshot_iso = next();
      } else if (arg == "--expiry") {
        options.source.initial_expiry = next();
      } else if (arg == "--rate") {
        options.source.rate = std::stod(next());
      } else if (arg == "--frames") {
        options.frame_limit = static_cast<std::size_t>(std::stoull(next()));
      } else if (arg == "--headless") {
        options.headless = true;
      } else if (arg == "--help" || arg == "-h") {
        options.help = true;
      } else {
        throw std::runtime_error("unknown argument: " + std::string(arg));
      }
    }
  } catch (const std::exception &error) {
    std::cerr << "argument error: " << error.what() << '\n';
    print_usage();
    return false;
  }
  return true;
}

void hide_console_window() {
#if defined(_WIN32)
  if (HWND console = GetConsoleWindow(); console != nullptr) {
    ShowWindow(console, SW_HIDE);
  }
#endif
}

} // namespace

int main(int argc, char **argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    return 1;
  }
  if (options.help) {
    print_usage();
    return 0;
  }

  atx::ui::OpraVolSurface source;
  const bool loaded = source.load(options.source);
  if (options.headless) {
    if (!loaded) {
      std::cerr << source.error() << '\n';
      return 1;
    }
    const atx::ui::VolCurveSlice &slice = source.slice();
    std::cout << "ATX_UI_SMOKE_OK symbol=" << slice.symbol << " snapshot=" << slice.snapshot_iso
              << " expiry=" << slice.expiry_iso << " contracts=" << source.contract_count()
              << " expiries=" << source.expiries().size() << " quotes=" << slice.quotes.size()
              << " curve_points=" << slice.curve.size() << " spot=" << slice.spot
              << " forward=" << slice.forward << " atm_vol=" << slice.atm_vol
              << " fit_ms=" << source.fit_milliseconds() << '\n';
    return 0;
  }

  hide_console_window();

  atx::ui::VolWorkspace workspace{source};
  atx::ui::ApplicationConfig app_config;
  app_config.title = "ATX VOL / " + options.source.symbol + " WORKSPACE";
  app_config.layout_id = "vol-surface-v2";
  app_config.frame_limit = options.frame_limit;
  atx::ui::Application app{std::move(app_config)};
  app.set_status_text(loaded ? options.source.symbol + " / HFT SURFACE READY" : "DATA ERROR");
  app.add_panel(atx::ui::PanelSpec{
      .id = "market_strip",
      .title = options.source.symbol + " SYMBOL VIEWER",
      .initial_dock = atx::ui::DockSlot::Top,
      .render = [&workspace] { workspace.render_market_strip(); },
  });
  app.add_panel(atx::ui::PanelSpec{
      .id = "curve_controls",
      .title = "CURVE CONTROLS",
      .initial_dock = atx::ui::DockSlot::Left,
      .render = [&workspace] { workspace.render_market_panel(); },
  });
  app.add_panel(atx::ui::PanelSpec{
      .id = "vol_curve",
      .title = "VOLATILITY SLICE",
      .initial_dock = atx::ui::DockSlot::Center,
      .render = [&workspace] { workspace.render_curve_panel(); },
  });
  app.add_panel(atx::ui::PanelSpec{
      .id = "fit_inspector",
      .title = "FIT INSPECTOR",
      .initial_dock = atx::ui::DockSlot::Right,
      .render = [&workspace] { workspace.render_fit_panel(); },
  });
  app.add_panel(atx::ui::PanelSpec{
      .id = "quote_ladder",
      .title = "OTM STRIKE LADDER",
      .initial_dock = atx::ui::DockSlot::LeftBottom,
      .render = [&workspace] { workspace.render_quote_panel(); },
  });
  app.add_panel(atx::ui::PanelSpec{
      .id = "term_structure",
      .title = "ATM TERM STRUCTURE",
      .initial_dock = atx::ui::DockSlot::Bottom,
      .render = [&workspace] { workspace.render_term_panel(); },
  });
  return app.run();
}
