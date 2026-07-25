// PY-F / Y2 — the FIT FRONT-END.
//
// The umbrella's blessed lifecycle is chain -> fit -> priced surface -> archive
// -> book (`vol.hpp`). Python could previously only enter it half-way, at a
// hand-authored parametric `PricedSurface`: fitting a surface from quotes — the
// product's actual pitch — had no Python surface at all. This TU binds the
// missing front half:
//
//   QuoteFrame (numpy columns) -> OptionChain::from_frame -> PricerFitter::fit
//     -> PricerFitter::value_chain(OutputField)  -> numpy SoA
//     -> FittedSurface::session().to_priced_surface() -> the existing API
//
// GIL: `fit` and `value_chain` are pure C++ over const state and fan out across
// their own workers, so both release it — the pattern `run_backtest` already
// proves. `value_chain` is documented bit-identical for any thread count, and
// `test_fit.py` drives that invariant FROM PYTHON, because a binding that fanned
// out through its own pool would break it invisibly.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "atx/vol/american.hpp"
#include "atx/vol/chain.hpp"
#include "atx/vol/curve.hpp"
#include "atx/vol/data.hpp"
#include "atx/vol/market_env.hpp"
#include "atx/vol/panel.hpp"
#include "atx/vol/pricer_fitter.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/session.hpp"
#include "atx/vol/spy_fixture.hpp"
#include "atx/vol/types.hpp"
#include "atx/vol/vol_curve.hpp"
#include "result.hpp"

namespace py = pybind11;
using namespace atx::vol;

namespace {

using DoubleArray = py::array_t<double, py::array::c_style | py::array::forcecast>;
using IdArray = py::array_t<std::uint64_t, py::array::c_style | py::array::forcecast>;
using IntArray = py::array_t<std::int32_t, py::array::c_style | py::array::forcecast>;

template <class T> py::array_t<T> to_array(const std::vector<T> &values) {
  py::array_t<T> out(static_cast<py::ssize_t>(values.size()));
  if (!values.empty()) {
    std::copy(values.begin(), values.end(), out.mutable_data());
  }
  return out;
}

// One AmericanGreeks field lifted into its own column. `value_chain` returns AoS
// greeks; a numpy consumer wants SoA, and doing the transpose here keeps the
// Python side from paying a per-row attribute lookup.
py::array_t<double> greek_column(const std::vector<AmericanGreeks> &greeks,
                                 double AmericanGreeks::*field) {
  py::array_t<double> out(static_cast<py::ssize_t>(greeks.size()));
  auto *data = out.mutable_data();
  for (std::size_t i = 0; i < greeks.size(); ++i) {
    data[i] = greeks[i].*field;
  }
  return out;
}

// The full SoA valuation as a dict of numpy arrays. Unrequested columns come
// back EMPTY rather than zero-filled, so a caller can never mistake a field it
// did not ask for for a field that came back zero.
py::dict valuation_to_dict(const ChainValuation &v) {
  py::dict out;
  out["ids"] = to_array(v.ids);
  out["model_price"] = to_array(v.model_price);
  out["model_iv"] = to_array(v.model_iv);
  out["bid_iv"] = to_array(v.bid_iv);
  out["ask_iv"] = to_array(v.ask_iv);
  out["mid_iv"] = to_array(v.mid_iv);
  out["delta"] = greek_column(v.greeks, &AmericanGreeks::delta);
  out["gamma"] = greek_column(v.greeks, &AmericanGreeks::gamma);
  out["vega"] = greek_column(v.greeks, &AmericanGreeks::vega);
  out["theta"] = greek_column(v.greeks, &AmericanGreeks::theta);
  out["rho"] = greek_column(v.greeks, &AmericanGreeks::rho);
  out["vanna"] = greek_column(v.greeks, &AmericanGreeks::vanna);
  out["volga"] = greek_column(v.greeks, &AmericanGreeks::volga);
  out["charm"] = greek_column(v.greeks, &AmericanGreeks::charm);
  out["greek_price"] = greek_column(v.greeks, &AmericanGreeks::price);
  out["filled"] = py::cast(v.filled);
  out["n_bid_unset"] = v.n_bid_unset;
  out["n_ask_unset"] = v.n_ask_unset;
  out["n_bid_iv_fail"] = v.n_bid_iv_fail;
  out["n_ask_iv_fail"] = v.n_ask_iv_fail;
  return out;
}

std::span<const double> as_span(const DoubleArray &array, const char *name) {
  if (array.ndim() != 1) {
    throw py::value_error(std::string{name} + " must be a one-dimensional array");
  }
  return {array.data(), static_cast<std::size_t>(array.size())};
}

std::vector<OptionId> as_ids(const IdArray &array) {
  if (array.ndim() != 1) {
    throw py::value_error("ids must be a one-dimensional array");
  }
  const auto n = static_cast<std::size_t>(array.size());
  std::vector<OptionId> out(n);
  const auto *data = array.data();
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = static_cast<OptionId>(data[i]);
  }
  return out;
}

// QuoteFrame from caller-owned numpy columns — the product ingestion path. One
// row per (expiry, strike, side) triple; every column must be the same length.
QuoteFrame frame_from_arrays(std::string uid, std::string snapshot_iso, double spot, double rate,
                             const std::vector<std::string> &expiry_iso,
                             const DoubleArray &strike_array, const IntArray &side_array,
                             const DoubleArray &bid_array, const DoubleArray &ask_array,
                             const std::vector<DividendEvent> &divs) {
  const auto strike = as_span(strike_array, "strike");
  const auto bid = as_span(bid_array, "bid");
  const auto ask = as_span(ask_array, "ask");
  const auto n = expiry_iso.size();
  if (side_array.ndim() != 1) {
    throw py::value_error("side must be a one-dimensional array");
  }
  const auto n_side = static_cast<std::size_t>(side_array.size());
  if (strike.size() != n || bid.size() != n || ask.size() != n || n_side != n) {
    throw py::value_error("expiry_iso, strike, side, bid and ask must be the same length");
  }

  QuoteFrame frame;
  frame.uid = uid;
  frame.snapshot_iso = std::move(snapshot_iso);
  frame.snapshot_ts_ns = iso_to_ns(frame.snapshot_iso);
  frame.spot = spot;
  frame.spot_ts_ns = frame.snapshot_ts_ns;
  frame.divs = divs;
  frame.rows.reserve(n);
  const auto *sides = side_array.data();
  for (std::size_t i = 0; i < n; ++i) {
    QuoteRow row;
    row.uid = uid;
    row.expiry_iso = expiry_iso[i];
    row.strike = strike[i];
    row.side = sides[i] == static_cast<std::int32_t>(Side::Put) ? Side::Put : Side::Call;
    row.bid = bid[i];
    row.ask = ask[i];
    row.bid_size = 1;
    row.ask_size = 1;
    row.under_spot = spot;
    row.ts_ns = frame.snapshot_ts_ns;
    frame.rows.push_back(std::move(row));
  }
  // `data_install` FAILS LOUD on a frame with no yield curve (the calibrator
  // silently degenerates without one), so build the same two-pillar flat bracket
  // `make_synthetic_american_panel` does: strictly ascending, spanning the
  // expiry range, both pillars carrying `rate`.
  double t_min = 0.0;
  double t_max = 0.0;
  for (const std::string &iso : expiry_iso) {
    const double t = year_fraction(frame.snapshot_iso, iso);
    if (!(t > 0.0)) {
      continue;
    }
    t_min = (t_min == 0.0) ? t : std::min(t_min, t);
    t_max = std::max(t_max, t);
  }
  if (!(t_max > 0.0)) {
    throw py::value_error("no expiry in `expiry_iso` is after `snapshot_iso`");
  }
  const double t_lo = std::max(1.0e-3, t_min * 0.5);
  const double t_hi = std::max(t_lo * 2.0, t_max * 1.5);
  frame.yc_pillar_t = {t_lo, t_hi};
  frame.yc_pillar_r = {rate, rate};

  atxvol::python::unwrap(build_uid_list(frame));
  return frame;
}

// The known-truth synthetic SPY board the C++ lifecycle test fits, plus a ready
// MarketEnv (the flat overload would drop the spec's two cash dividends). This
// is what lets `test_fit.py` run with no external data anywhere.
struct SynthPanelPy {
  QuoteFrame frame;
  MarketEnv env;
  std::vector<double> truth_iv;
  std::vector<double> truth_forward;
};

SynthPanelPy make_spy_panel(const std::string &snapshot_iso) {
  const SynthPanelSpec spec = make_spy_synthetic_spec(snapshot_iso);
  auto panel = atxvol::python::unwrap(make_synthetic_american_panel(spec));
  return SynthPanelPy{std::move(panel.frame),
                      MarketEnv::flat(spec.spot, spec.r, iso_to_ns(spec.snapshot_iso),
                                      spec.cash_divs),
                      std::move(panel.truth_iv), std::move(panel.truth_forward)};
}

} // namespace

void bind_fit(py::module_ &m) {
  m.def("iso_to_ns", &iso_to_ns, py::arg("iso"),
        "Epoch nanoseconds for an ISO-8601 date or datetime (0 on parse failure).");

  py::class_<DividendEvent>(m, "DividendEvent")
      .def(py::init<>())
      .def(py::init([](std::int64_t ex_date_ns, double amount) {
             return DividendEvent{ex_date_ns, amount};
           }),
           py::arg("ex_date_ns"), py::arg("amount"))
      .def_readwrite("ex_date_ns", &DividendEvent::ex_date_ns)
      .def_readwrite("amount", &DividendEvent::amount);

  py::class_<MarketEnv>(m, "MarketEnv")
      .def(py::init<>())
      .def_static("flat", &MarketEnv::flat, py::arg("spot"), py::arg("r"), py::arg("now_ns"),
                  py::arg("divs") = std::vector<DividendEvent>{},
                  "The flat-rate environment — bit-identical to the historical scalar path.")
      .def_readwrite("spot", &MarketEnv::spot)
      .def_readwrite("now_ns", &MarketEnv::now_ns)
      .def_readwrite("flat_rate", &MarketEnv::flat_rate)
      .def_readwrite("cash_divs", &MarketEnv::cash_divs)
      .def("rate_at", &MarketEnv::rate_at, py::arg("T"));

  py::class_<QuoteFrame>(m, "QuoteFrame")
      .def(py::init<>())
      .def_static("from_arrays", &frame_from_arrays, py::arg("uid"), py::arg("snapshot_iso"),
                  py::arg("spot"), py::arg("rate"), py::arg("expiry_iso"), py::arg("strike"),
                  py::arg("side"), py::arg("bid"), py::arg("ask"),
                  py::arg("divs") = std::vector<DividendEvent>{},
                  "Build a quote frame from caller-owned columns: one row per\n"
                  "(expiry_iso, strike, side) triple. `side` is an int array of\n"
                  "`int(Side.CALL)` / `int(Side.PUT)`. Every column must be the\n"
                  "same length as `expiry_iso`. `rate` populates the flat yield\n"
                  "curve `data_install` requires (it fails loud without one).")
      .def("__len__", [](const QuoteFrame &self) { return self.rows.size(); })
      .def_readonly("uid", &QuoteFrame::uid)
      .def_readonly("snapshot_iso", &QuoteFrame::snapshot_iso)
      .def_readonly("snapshot_ts_ns", &QuoteFrame::snapshot_ts_ns)
      .def_readwrite("spot", &QuoteFrame::spot);

  py::class_<SynthPanelPy>(m, "SynthPanel")
      .def_readonly("frame", &SynthPanelPy::frame)
      .def_readonly("env", &SynthPanelPy::env)
      .def_property_readonly(
          "truth_iv", [](const SynthPanelPy &self) { return to_array(self.truth_iv); })
      .def_property_readonly(
          "truth_forward",
          [](const SynthPanelPy &self) { return to_array(self.truth_forward); });

  m.def("make_spy_synthetic_panel", &make_spy_panel, py::arg("snapshot_iso") = "2026-06-19",
        "The deterministic known-truth synthetic SPY board (no external data),\n"
        "with a MarketEnv carrying its two cash dividends.");

  py::class_<OptionRef>(m, "OptionRef")
      .def_readonly("id", &OptionRef::id)
      .def_readonly("expiry_ns", &OptionRef::expiry_ns)
      .def_readonly("T", &OptionRef::T)
      .def_readonly("strike", &OptionRef::strike)
      .def_readonly("side", &OptionRef::side)
      .def_readonly("bid", &OptionRef::bid)
      .def_readonly("ask", &OptionRef::ask)
      .def_readonly("mid", &OptionRef::mid)
      .def_readonly("bid_size", &OptionRef::bid_size)
      .def_readonly("ask_size", &OptionRef::ask_size);

  py::class_<OptionChain>(m, "OptionChain")
      .def_static(
          "from_frame",
          [](const QuoteFrame &frame, MarketEnv env) {
            return atxvol::python::unwrap(OptionChain::from_frame(frame, std::move(env)));
          },
          py::arg("frame"), py::arg("env"),
          "Install `frame` and resolve its single underlying, carrying the full\n"
          "MarketEnv (spot / rate curve / dividends / valuation time).")
      .def_static(
          "from_frame_flat",
          [](const QuoteFrame &frame, double r, double spot) {
            return atxvol::python::unwrap(OptionChain::from_frame(frame, r, spot));
          },
          py::arg("frame"), py::arg("r"), py::arg("spot") = 0.0,
          "Legacy flat-scalar overload; `spot` overrides the frame's when > 0.")
      .def("__len__", &OptionChain::size)
      .def("size", &OptionChain::size)
      .def_property_readonly("spot", &OptionChain::spot)
      .def_property_readonly("rate", &OptionChain::rate)
      .def_property_readonly("now_ns", &OptionChain::now_ns)
      .def_property_readonly("instance_id", &OptionChain::instance_id)
      .def_property_readonly("quote_revision", &OptionChain::quote_revision)
      .def("ids",
           [](const OptionChain &self) {
             const std::vector<OptionId> ids = self.ids();
             py::array_t<std::uint64_t> out(static_cast<py::ssize_t>(ids.size()));
             auto *data = out.mutable_data();
             for (std::size_t i = 0; i < ids.size(); ++i) {
               data[i] = static_cast<std::uint64_t>(ids[i]);
             }
             return out;
           },
           "Every option id, in a stable deterministic order (ascending expiry,\n"
           "strike, then side) that is independent of quote content.")
      .def("at",
           [](const OptionChain &self, std::uint64_t id) {
             return atxvol::python::unwrap(self.at(static_cast<OptionId>(id)));
           },
           py::arg("id"))
      .def("snapshot",
           [](const OptionChain &self) {
             ChainSnapshot snap = self.snapshot();
             py::dict out;
             std::vector<std::uint64_t> ids(snap.ids.size());
             for (std::size_t i = 0; i < snap.ids.size(); ++i) {
               ids[i] = static_cast<std::uint64_t>(snap.ids[i]);
             }
             std::vector<std::int32_t> sides(snap.side.size());
             for (std::size_t i = 0; i < snap.side.size(); ++i) {
               sides[i] = static_cast<std::int32_t>(snap.side[i]);
             }
             out["ids"] = to_array(ids);
             out["T"] = to_array(snap.T);
             out["strike"] = to_array(snap.strike);
             out["bid"] = to_array(snap.bid);
             out["ask"] = to_array(snap.ask);
             out["mid"] = to_array(snap.mid);
             out["side"] = to_array(sides);
             return out;
           },
           "Flatten every valuation-relevant field into numpy columns aligned\n"
           "with ids(). Detached from later quote updates.")
      .def("update_quotes",
           [](OptionChain &self, const IdArray &ids, const DoubleArray &bids,
              const DoubleArray &asks) {
             const std::vector<OptionId> id_vec = as_ids(ids);
             const auto bid = as_span(bids, "bids");
             const auto ask = as_span(asks, "asks");
             atxvol::python::unwrap(self.update_quotes(id_vec, bid, ask));
           },
           py::arg("ids"), py::arg("bids"), py::arg("asks"),
           "Replace bid/ask for a batch of ids (mids recomputed). Ids that do\n"
           "not decode to a known leg are silently dropped, matching the engine.");

  // OutputField is a bit set; `py::arithmetic()` gives Python the `|` the C++
  // API is driven by, so a caller writes ModelIV | Greeks exactly as in C++.
  // pybind11 gives a SCOPED enum only comparison operators under
  // `py::arithmetic()` (see enum_base::init: the bitwise ops are on the
  // is_convertible branch, which an `enum class` does not take). OutputField is
  // a bit set driven by `|` in every C++ call site, so wire the operators
  // explicitly — a Python caller writes `ModelIV | Greeks` exactly as in C++.
  py::enum_<OutputField> output_field(m, "OutputField", py::arithmetic());
  output_field.def("__or__", [](OutputField a, OutputField b) { return a | b; },
                   py::is_operator());
  output_field.def("__and__", [](OutputField a, OutputField b) { return a & b; },
                   py::is_operator());
  output_field.def(
      "has", [](OutputField self, OutputField flag) { return has(self, flag); },
      py::arg("flag"), "True iff `flag` is set in this field set.");
  output_field
      .value("None_", OutputField::None)
      .value("MODEL_PRICE", OutputField::ModelPrice)
      .value("MODEL_IV", OutputField::ModelIV)
      .value("BID_IV", OutputField::BidIV)
      .value("ASK_IV", OutputField::AskIV)
      .value("MID_IV", OutputField::MidIV)
      .value("GREEKS", OutputField::Greeks)
      .value("Prices", OutputField::Prices)
      .value("Bands", OutputField::Bands)
      .value("All", OutputField::All);

  py::enum_<FitPreset>(m, "FitPreset")
      .value("FAST", FitPreset::Fast)
      .value("ACCURATE", FitPreset::Accurate)
      .value("ROBUST", FitPreset::Robust)
      .value("HFT", FitPreset::Hft)
      .value("POPULATE", FitPreset::Populate);

  py::enum_<SurfacePurpose>(m, "SurfacePurpose")
      .value("MARKET_MARK", SurfacePurpose::MarketMark)
      .value("RISK", SurfacePurpose::Risk);

  py::class_<PricerConfig>(m, "PricerConfig")
      .def(py::init<>())
      .def_readwrite("preset", &PricerConfig::preset)
      .def_readwrite("n_threads", &PricerConfig::n_threads)
      .def_readwrite("query_pricing_tier", &PricerConfig::query_pricing_tier)
      // `curve` is `optional<CurveConfig>`, a deep struct whose knobs are not
      // (yet) bound. This property is the pin/unpin switch a Python caller
      // actually needs: assign a VolCurveKind to pin that family with its
      // defaults, or None to hand the board back to the auto-routing policy.
      .def_property(
          "curve_kind",
          [](const PricerConfig &self) -> std::optional<VolCurveKind> {
            if (!self.curve.has_value()) {
              return std::nullopt;
            }
            return self.curve->kind;
          },
          [](PricerConfig &self, std::optional<VolCurveKind> kind) {
            if (!kind.has_value()) {
              self.curve.reset();
              return;
            }
            if (!self.curve.has_value()) {
              self.curve = CurveConfig{};
            }
            self.curve->kind = *kind;
          },
          "Pin the curve family (None => the profile policy routes the board).");

  // SHARED holder, deliberately (rev-ws-y C1). `PricerFitter` owns its surfaces
  // as `shared_ptr<const FittedSurface>` and `fit()` REPLACES the stored
  // generation, so a Python handle has to be a CO-OWNER of the generation it was
  // handed. The default unique holder plus `reference_internal` cannot express
  // that: `reference_internal` is `reference` + `keep_alive<0,1>`, which keeps
  // the FITTER alive and nothing at all keeps the GENERATION alive — the next
  // `fit()` drops the last reference and the live handle dangles (a four-line
  // reproducible access violation, `test_fit.py`). `keep_alive` is structurally
  // the wrong tool here: the fitter legitimately outlives the generation.
  //
  // The holder is a per-TYPE decision in pybind11, so every binding that
  // returns, accepts or stores a `FittedSurface` must agree — `surface()` below
  // is the only one today; a future `risk_surface()` / `market_mark_surface()` /
  // `bundle()` must hand back the owning `shared_ptr` the same way.
  py::class_<FittedSurface, std::shared_ptr<FittedSurface>>(m, "FittedSurface")
      .def("iv", &FittedSurface::iv, py::arg("K"), py::arg("T"),
           py::call_guard<py::gil_scoped_release>())
      .def(
          "fair_value",
          [](const FittedSurface &self, double k, double t, Side side) {
            return atxvol::python::unwrap(self.fair_value(k, t, side));
          },
          py::arg("K"), py::arg("T"), py::arg("side"), py::call_guard<py::gil_scoped_release>())
      .def(
          "greeks",
          [](const FittedSurface &self, double k, double t, Side side) {
            return atxvol::python::unwrap(self.greeks(k, t, side));
          },
          py::arg("K"), py::arg("T"), py::arg("side"), py::call_guard<py::gil_scoped_release>())
      .def_property_readonly("purpose", &FittedSurface::purpose)
      .def_property_readonly("generation", &FittedSurface::generation)
      .def(
          "to_priced_surface",
          [](const FittedSurface &self) {
            return atxvol::python::unwrap(self.session().to_priced_surface());
          },
          py::call_guard<py::gil_scoped_release>(),
          "Seal this fit into an owned PricedSurface — the hand-off into the\n"
          "archive / SurfaceDb / backtest half of the lifecycle.");

  py::class_<PricerFitter>(m, "PricerFitter")
      .def(py::init<PricerConfig>(), py::arg("config") = PricerConfig{})
      .def(
          "fit",
          [](PricerFitter &self, const OptionChain &chain) {
            atxvol::python::unwrap(self.fit(chain));
          },
          py::arg("chain"), py::call_guard<py::gil_scoped_release>(),
          "Fit the surface from `chain` and store it, replacing any prior fit.\n"
          "The prior surface is left intact on failure.")
      .def_property_readonly("fitted", &PricerFitter::fitted)
      .def("set_threads", &PricerFitter::set_threads, py::arg("n"))
      .def(
          "surface",
          [](const PricerFitter &self) {
            // Hand back the OWNER, not the observer. `surface()` returns a raw
            // pointer into whichever generation the fail-closed default-purpose
            // routing selected; `bundle()` carries the matching shared_ptr
            // leases, so recover the owning handle by identity rather than
            // re-deriving the routing rule here (which would then have two
            // places to disagree). The const_cast is at the binding seam only:
            // every bound method on FittedSurface is a const query.
            const FittedSurface *raw = self.surface();
            if (raw == nullptr) {
              throw atxvol::python::AtxException(atx::core::Error{
                  atx::core::ErrorCode::Unavailable,
                  "no surface is served for the config's default purpose"});
            }
            const SurfaceBundle bundle = self.bundle();
            std::shared_ptr<const FittedSurface> owner;
            if (bundle.risk.get() == raw) {
              owner = bundle.risk;
            } else if (bundle.market_mark.get() == raw) {
              owner = bundle.market_mark;
            } else {
              throw atxvol::python::AtxException(atx::core::Error{
                  atx::core::ErrorCode::Internal,
                  "served surface is not one of the fitter's published leases"});
            }
            return std::const_pointer_cast<FittedSurface>(owner);
          },
          "The served surface for the config's default purpose.\n\n"
          "The returned handle CO-OWNS its generation: a later `fit()` publishes\n"
          "a new generation without invalidating a handle a caller still holds,\n"
          "and the handle stays valid after the fitter itself is dropped.")
      .def(
          "value_chain",
          [](const PricerFitter &self, const OptionChain &chain, OutputField fields,
             unsigned n_threads) {
            ChainValuation valuation;
            {
              py::gil_scoped_release release;
              valuation =
                  atxvol::python::unwrap(self.value_chain(chain, fields, n_threads));
            }
            return valuation_to_dict(valuation);
          },
          py::arg("chain"), py::arg("fields"), py::arg("n_threads") = 0,
          "Price the chain's options for `fields` as numpy SoA columns.\n\n"
          "DETERMINISTIC: bit-identical for any `n_threads` (disjoint output\n"
          "slots, pure const reads). 0 => the config's n_threads; 1 => serial.\n"
          "Unrequested columns come back EMPTY, never zero-filled.")
      .def(
          "value_chain_ids",
          [](const PricerFitter &self, const OptionChain &chain, const IdArray &ids,
             OutputField fields, unsigned n_threads) {
            const std::vector<OptionId> selected = as_ids(ids);
            ChainValuation valuation;
            {
              py::gil_scoped_release release;
              valuation = atxvol::python::unwrap(
                  self.value_chain(chain, selected, fields, n_threads));
            }
            return valuation_to_dict(valuation);
          },
          py::arg("chain"), py::arg("ids"), py::arg("fields"), py::arg("n_threads") = 0,
          "Price only `ids`, preserving caller order and duplicates — the\n"
          "quote-update path, with work proportional to the selection.");
}
