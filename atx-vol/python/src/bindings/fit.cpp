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
// GIL: `fit` and `value_chain` are both long C++ calls that fan out across their
// own workers, so both release it — the pattern `run_backtest` already proves.
// `fit` is NOT const, though: `pricer_fitter.hpp` states that it "mutates
// (stores the surface) and needs exclusive access", while `value_chain` is const
// and safe to run concurrently. Once `value_chain` runs GIL-free the GIL cannot
// supply that exclusivity, so the Python wrappers below carry `shared_mutex`es
// and the binding — not the interpreter — serializes both fitter mutation and
// OptionChain's "many readers OR one writer" contract. `value_chain` is
// documented bit-identical for any thread count, and `test_fit.py` drives that
// invariant FROM PYTHON, because a binding that fanned out through its own pool
// would break it invisibly.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
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
#include "sides.hpp"

namespace py = pybind11;
using namespace atx::vol;

namespace {

using DoubleArray = py::array_t<double, py::array::c_style | py::array::forcecast>;
using IdArray = py::array_t<std::uint64_t, py::array::c_style | py::array::forcecast>;

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
                             const DoubleArray &strike_array, const py::object &raw_side,
                             const DoubleArray &bid_array, const DoubleArray &ask_array,
                             const std::vector<DividendEvent> &divs) {
  // FIX-5 (final-review Minor): dtype kind validated before the cast (sides.hpp).
  const atxvol::python::SideCodes side_array = atxvol::python::as_side_codes(raw_side);
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
    // One shared decoder (I2). Rejecting here matters most: a board imported
    // with a +1/-1 convention would otherwise be INSTALLED with every leg as a
    // call, and every number downstream would be silently wrong.
    row.side = atxvol::python::decode_side(sides[i], i);
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
  // The floor must never rise ABOVE the shortest expiry, or the bracket stops
  // spanning the range the comment above promises (M5). The old 1e-3 floor did
  // exactly that for a 0DTE/1DTE board — any `t_min` under ~2e-3 y (≈17 h). The
  // curve is flat, so this changes no number on a board that already spanned;
  // it makes the invariant the comment asserts the one the code enforces.
  const double t_lo = std::max(1.0e-9, t_min * 0.5);
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

// ── Binding-side reader/writer serialization ───────────────────────────────
//
// `pricer_fitter.hpp:22-24` is explicit: "`fit` mutates (stores the surface) and
// needs exclusive access. `value_chain` is const and internally parallel;
// concurrent `value_chain` calls on one fitter are safe." The library STATES
// that requirement; it does not enforce it, and a Python caller has no way to.
//
// The GIL cannot supply the exclusivity here, which is what makes this different
// from `AloPricer::price` (PY-5, `pricing.cpp`, where holding the GIL genuinely
// is the whole synchronization story). `value_chain` is long and genuinely
// parallel, so it must release the GIL — and once one side of a mutator/reader
// pair runs GIL-free, the GIL serializes nothing. `fit` reassigning the
// non-atomic `shared_ptr market_mark_surface_` while `value_chain` copies it is
// a data race whose failure mode is refcount corruption; `test_fit.py` drives it
// out of process and it dies with an access violation.
//
// `chain.hpp` independently requires many readers OR one writer. `fit` and
// `value_chain` release the GIL while reading a chain, so a Python thread can
// otherwise enter `update_quotes` and race the Universe's quote vectors and
// revision state. PyOptionChain makes the library's external synchronization
// requirement an invariant of the Python object: every accessor takes shared
// access and mutation takes exclusive access.
//
// LOCK ORDER: a call needing both objects takes PyPricerFitter::mu_ first, then
// PyOptionChain::mu_. No code may wait for the fitter while holding the chain.
// Every potentially blocking lock is taken with the GIL ALREADY RELEASED, and
// every lock is destroyed before `gil_scoped_release`, so the GIL is reacquired
// last. This prevents both lock-order inversion and an interpreter-wide stall.
class PyOptionChain {
public:
  explicit PyOptionChain(OptionChain chain) : chain_(std::move(chain)) {}

  PyOptionChain(const PyOptionChain &) = delete;
  PyOptionChain &operator=(const PyOptionChain &) = delete;
  PyOptionChain(PyOptionChain &&) = delete;
  PyOptionChain &operator=(PyOptionChain &&) = delete;

  template <class Fn> decltype(auto) with_read(Fn &&fn) const {
    const std::shared_lock<std::shared_mutex> lock(mu_);
    return std::forward<Fn>(fn)(chain_);
  }

  template <class Fn> decltype(auto) with_write(Fn &&fn) {
    const std::unique_lock<std::shared_mutex> lock(mu_);
    return std::forward<Fn>(fn)(chain_);
  }

private:
  OptionChain chain_;
  mutable std::shared_mutex mu_;
};

class PyPricerFitter {
public:
  explicit PyPricerFitter(PricerConfig cfg) : fitter_(std::move(cfg)) {}

  void fit(const PyOptionChain &chain) {
    py::gil_scoped_release release;
    const std::unique_lock<std::shared_mutex> fitter_lock(mu_);
    chain.with_read([this](const OptionChain &locked_chain) {
      atxvol::python::unwrap(fitter_.fit(locked_chain));
    });
  }

  [[nodiscard]] bool fitted() const {
    py::gil_scoped_release release;
    const std::shared_lock<std::shared_mutex> lock(mu_);
    return fitter_.fitted();
  }

  void set_threads(unsigned n) {
    py::gil_scoped_release release;
    const std::unique_lock<std::shared_mutex> lock(mu_);
    fitter_.set_threads(n);
  }

  // Hands back the OWNER of the served generation, not an observer of it: see
  // the FittedSurface holder note in `bind_fit`. `surface()` returns a raw
  // pointer into whichever generation the fail-closed default-purpose routing
  // selected, and `bundle()` carries the matching shared_ptr leases, so the
  // owning handle is recovered by identity rather than by re-deriving the
  // routing rule here (which would then have two places to disagree). The
  // const_cast is at the binding seam only: every bound method is a const query.
  [[nodiscard]] std::shared_ptr<FittedSurface> surface() const {
    std::shared_ptr<const FittedSurface> owner;
    bool served = false;
    {
      py::gil_scoped_release release;
      const std::shared_lock<std::shared_mutex> lock(mu_);
      const FittedSurface *raw = fitter_.surface();
      served = raw != nullptr;
      if (served) {
        const SurfaceBundle bundle = fitter_.bundle();
        if (bundle.risk.get() == raw) {
          owner = bundle.risk;
        } else if (bundle.market_mark.get() == raw) {
          owner = bundle.market_mark;
        }
      }
    }
    if (!served) {
      throw atxvol::python::AtxException(
          atx::core::Error{atx::core::ErrorCode::Unavailable,
                           "no surface is served for the config's default purpose"});
    }
    if (!owner) {
      throw atxvol::python::AtxException(
          atx::core::Error{atx::core::ErrorCode::Internal,
                           "served surface is not one of the fitter's published leases"});
    }
    return std::const_pointer_cast<FittedSurface>(owner);
  }

  [[nodiscard]] py::dict value_chain(const PyOptionChain &chain, OutputField fields,
                                     unsigned n_threads) const {
    ChainValuation valuation;
    {
      py::gil_scoped_release release;
      const std::shared_lock<std::shared_mutex> fitter_lock(mu_);
      valuation = chain.with_read([this, fields, n_threads](const OptionChain &locked_chain) {
        return atxvol::python::unwrap(fitter_.value_chain(locked_chain, fields, n_threads));
      });
    }
    return valuation_to_dict(valuation);
  }

  [[nodiscard]] py::dict value_chain_ids(const PyOptionChain &chain, const IdArray &ids,
                                         OutputField fields, unsigned n_threads) const {
    // Decoded with the GIL still held: `as_ids` reads a numpy object.
    const std::vector<OptionId> selected = as_ids(ids);
    ChainValuation valuation;
    {
      py::gil_scoped_release release;
      const std::shared_lock<std::shared_mutex> fitter_lock(mu_);
      valuation =
          chain.with_read([this, &selected, fields, n_threads](const OptionChain &locked_chain) {
            return atxvol::python::unwrap(
                fitter_.value_chain(locked_chain, selected, fields, n_threads));
          });
    }
    return valuation_to_dict(valuation);
  }

private:
  PricerFitter fitter_;
  mutable std::shared_mutex mu_;
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

  py::class_<YieldCurve>(m, "YieldCurve")
      .def(py::init<>(), "An empty curve; MarketEnv falls back to flat_rate.")
      .def_static(
          "create",
          [](const std::vector<double> &t_years,
             const std::vector<double> &zero_rates) {
            for (double t : t_years) {
              if (!std::isfinite(t) || t <= 0.0) {
                throw atxvol::python::AtxException(atx::core::Error{
                    atx::core::ErrorCode::InvalidArgument,
                    "YieldCurve.create: maturities must be finite and positive"});
              }
            }
            for (double rate : zero_rates) {
              if (!std::isfinite(rate)) {
                throw atxvol::python::AtxException(atx::core::Error{
                    atx::core::ErrorCode::InvalidArgument,
                    "YieldCurve.create: zero rates must be finite"});
              }
            }
            return atxvol::python::unwrap(
                YieldCurve::create(t_years, zero_rates));
          },
          py::arg("t_years"), py::arg("zero_rates"),
          "Build a validated Fritsch-Carlson term curve. Pillar maturities must "
          "be finite, non-empty and strictly increasing.")
      .def("zero", &YieldCurve::zero, py::arg("T"))
      .def("disc", &YieldCurve::disc, py::arg("T"))
      .def("__len__", &YieldCurve::size)
      .def_property_readonly("size", &YieldCurve::size);

  py::class_<MarketEnv>(m, "MarketEnv")
      .def(py::init<>())
      .def_static("flat", &MarketEnv::flat, py::arg("spot"), py::arg("r"), py::arg("now_ns"),
                  py::arg("divs") = std::vector<DividendEvent>{},
                  "The flat-rate environment — bit-identical to the historical scalar path.")
      .def_readwrite("spot", &MarketEnv::spot)
      .def_readwrite("now_ns", &MarketEnv::now_ns)
      .def_readwrite("flat_rate", &MarketEnv::flat_rate)
      .def_readwrite("yield_curve", &MarketEnv::yield,
                     "Optional validated term curve. An empty curve selects flat_rate.")
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

  py::class_<PyOptionChain>(m, "OptionChain")
      .def_static(
          "from_frame",
          [](const QuoteFrame &frame, MarketEnv env) {
            return std::make_unique<PyOptionChain>(
                atxvol::python::unwrap(OptionChain::from_frame(frame, std::move(env))));
          },
          py::arg("frame"), py::arg("env"),
          "Install `frame` and resolve its single underlying, carrying the full\n"
          "MarketEnv (spot / rate curve / dividends / valuation time).")
      .def_static(
          "from_frame_flat",
          [](const QuoteFrame &frame, double r, double spot) {
            return std::make_unique<PyOptionChain>(
                atxvol::python::unwrap(OptionChain::from_frame(frame, r, spot)));
          },
          py::arg("frame"), py::arg("r"), py::arg("spot") = 0.0,
          "Legacy flat-scalar overload; `spot` overrides the frame's when > 0.")
      .def("__len__",
           [](const PyOptionChain &self) {
             py::gil_scoped_release release;
             return self.with_read([](const OptionChain &chain) { return chain.size(); });
           })
      .def("size",
           [](const PyOptionChain &self) {
             py::gil_scoped_release release;
             return self.with_read([](const OptionChain &chain) { return chain.size(); });
           })
      .def_property_readonly("spot",
                             [](const PyOptionChain &self) {
                               py::gil_scoped_release release;
                               return self.with_read(
                                   [](const OptionChain &chain) { return chain.spot(); });
                             })
      .def_property_readonly("rate",
                             [](const PyOptionChain &self) {
                               py::gil_scoped_release release;
                               return self.with_read(
                                   [](const OptionChain &chain) { return chain.rate(); });
                             })
      .def_property_readonly("now_ns",
                             [](const PyOptionChain &self) {
                               py::gil_scoped_release release;
                               return self.with_read(
                                   [](const OptionChain &chain) { return chain.now_ns(); });
                             })
      .def_property_readonly("instance_id",
                             [](const PyOptionChain &self) {
                               py::gil_scoped_release release;
                               return self.with_read(
                                   [](const OptionChain &chain) { return chain.instance_id(); });
                             })
      .def_property_readonly("quote_revision",
                             [](const PyOptionChain &self) {
                               py::gil_scoped_release release;
                               return self.with_read(
                                   [](const OptionChain &chain) { return chain.quote_revision(); });
                             })
      .def(
          "ids",
          [](const PyOptionChain &self) {
            std::vector<OptionId> ids;
            {
              py::gil_scoped_release release;
              ids = self.with_read([](const OptionChain &chain) { return chain.ids(); });
            }
            py::array_t<std::uint64_t> out(static_cast<py::ssize_t>(ids.size()));
            auto *data = out.mutable_data();
            for (std::size_t i = 0; i < ids.size(); ++i) {
              data[i] = static_cast<std::uint64_t>(ids[i]);
            }
            return out;
          },
          "Every option id, in a stable deterministic order (ascending expiry,\n"
          "strike, then side) that is independent of quote content.")
      .def(
          "at",
          [](const PyOptionChain &self, std::uint64_t id) {
            py::gil_scoped_release release;
            return self.with_read([id](const OptionChain &chain) {
              return atxvol::python::unwrap(chain.at(static_cast<OptionId>(id)));
            });
          },
          py::arg("id"))
      .def(
          "snapshot",
          [](const PyOptionChain &self) {
            ChainSnapshot snap;
            {
              py::gil_scoped_release release;
              snap = self.with_read([](const OptionChain &chain) { return chain.snapshot(); });
            }
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
      .def(
          "update_quotes",
          [](PyOptionChain &self, const IdArray &ids, const DoubleArray &bids,
             const DoubleArray &asks) {
            const std::vector<OptionId> id_vec = as_ids(ids);
            const auto bid = as_span(bids, "bids");
            const auto ask = as_span(asks, "asks");
            py::gil_scoped_release release;
            self.with_write([&](OptionChain &chain) {
              atxvol::python::unwrap(chain.update_quotes(id_vec, bid, ask));
            });
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

  // F-3: expose the actual CurveConfig tree, not only its family tag. Every
  // value-owning production knob is writable; SplineFitOpts::grid remains the
  // library's fixed standard grid because that C++ member is a borrowed span.
  py::enum_<EssviRhoMode>(m, "EssviRhoMode")
      .value("PER_SLICE", EssviRhoMode::PerSlice)
      .value("SHARED", EssviRhoMode::Shared)
      .value("TERM_STRUCTURE", EssviRhoMode::TermStructure);
  py::enum_<OptimizationLevel>(m, "OptimizationLevel")
      .value("QUICK_MARK", OptimizationLevel::QuickMark)
      .value("TRADING", OptimizationLevel::Trading)
      .value("RISK", OptimizationLevel::Risk)
      .value("REFERENCE", OptimizationLevel::Reference)
      .value("COLD_FAST", OptimizationLevel::ColdFast);
  py::enum_<CalibLossKind>(m, "CalibLossKind")
      .value("MID", CalibLossKind::Mid)
      .value("INTERVAL", CalibLossKind::Interval);
  py::enum_<CalibAnchorKind>(m, "CalibAnchorKind")
      .value("MID", CalibAnchorKind::Mid)
      .value("BID", CalibAnchorKind::Bid)
      .value("ASK", CalibAnchorKind::Ask);
  py::class_<ConvexFitOpts>(m, "ConvexFitOpts")
      .def(py::init<>())
      .def_readwrite("lambda_", &ConvexFitOpts::lambda)
      .def_readwrite("bound_slope_below", &ConvexFitOpts::bound_slope_below)
      .def_readwrite("node_cap", &ConvexFitOpts::node_cap)
      .def_readwrite("max_iter", &ConvexFitOpts::max_iter)
      .def_readwrite("loss", &ConvexFitOpts::loss);

  py::class_<CalibOpts>(m, "CalibOpts")
      .def(py::init<>())
      .def_readwrite("max_outer_iter", &CalibOpts::max_outer_iter)
      .def_readwrite("max_inner_iter", &CalibOpts::max_inner_iter)
      .def_readwrite("tol_param", &CalibOpts::tol_param)
      .def_readwrite("tol_residual", &CalibOpts::tol_residual)
      .def_readwrite("huber_k", &CalibOpts::huber_k)
      .def_readwrite("min_vega_weight", &CalibOpts::min_vega_weight)
      .def_readwrite("max_spread_vol", &CalibOpts::max_spread_vol)
      .def_readwrite("max_weight", &CalibOpts::max_weight)
      .def_readwrite("max_obs_per_slice", &CalibOpts::max_obs_per_slice)
      .def_readwrite("warm_start_deam_adjacent_strikes",
                     &CalibOpts::warm_start_deam_adjacent_strikes)
      .def_readwrite("use_shared_boundary_deam",
                     &CalibOpts::use_shared_boundary_deam)
      .def_readwrite("max_deam_strikes_per_expiry",
                     &CalibOpts::max_deam_strikes_per_expiry)
      .def_readwrite("max_otm_shortcut_premium_spread_frac",
                     &CalibOpts::max_otm_shortcut_premium_spread_frac)
      .def_readwrite("max_inversion_residual_half_spreads",
                     &CalibOpts::max_inversion_residual_half_spreads)
      .def_readwrite("audit_accurate_inversions",
                     &CalibOpts::audit_accurate_inversions)
      .def_readwrite("min_otm_shortcut_T", &CalibOpts::min_otm_shortcut_T)
      .def_readwrite("min_otm_shortcut_vega", &CalibOpts::min_otm_shortcut_vega)
      .def_readwrite("max_otm_shortcut_abs_k", &CalibOpts::max_otm_shortcut_abs_k)
      .def_readwrite("max_certified_deam_drop_fraction",
                     &CalibOpts::max_certified_deam_drop_fraction)
      .def_readwrite("prior_strength", &CalibOpts::prior_strength)
      .def_readwrite("essvi_rho_mode", &CalibOpts::essvi_rho_mode)
      .def_readwrite("optimization_level", &CalibOpts::optimization_level)
      .def_readwrite("essvi_fallback_rmse_threshold",
                     &CalibOpts::essvi_fallback_rmse_threshold)
      .def_readwrite("n_butterfly_grid", &CalibOpts::n_butterfly_grid)
      .def_readwrite("max_iter_quick_mark", &CalibOpts::max_iter_quick_mark)
      .def_readwrite("max_iter_trading", &CalibOpts::max_iter_trading)
      .def_readwrite("max_iter_risk", &CalibOpts::max_iter_risk)
      .def_readwrite("max_iter_reference", &CalibOpts::max_iter_reference)
      .def_readwrite("max_iter_cold_fast", &CalibOpts::max_iter_cold_fast)
      .def_readwrite("wing_floor_alpha", &CalibOpts::wing_floor_alpha)
      .def_readwrite("lee_bound_project", &CalibOpts::lee_bound_project)
      .def_readwrite("morozov_stop", &CalibOpts::morozov_stop)
      .def_readwrite("morozov_tau", &CalibOpts::morozov_tau)
      .def_readwrite("validate_no_arb", &CalibOpts::validate_no_arb)
      .def_readwrite("essvi_alt_driver_theta_project",
                     &CalibOpts::essvi_alt_driver_theta_project)
      .def_readwrite("residual_disable", &CalibOpts::residual_disable)
      .def_readwrite("residual_basis_kind", &CalibOpts::residual_basis_kind)
      .def_readwrite("residual_n_basis_terms", &CalibOpts::residual_n_basis_terms)
      .def_readwrite("residual_ridge_factor", &CalibOpts::residual_ridge_factor)
      .def_readwrite("loss_kind", &CalibOpts::loss_kind)
      .def_readwrite("anchor_kind", &CalibOpts::anchor_kind)
      .def_readwrite("essvi_asymmetric_rho", &CalibOpts::essvi_asymmetric_rho)
      .def_readwrite("min_obs_per_slice", &CalibOpts::min_obs_per_slice)
      .def_readwrite("max_post_fit_sigma", &CalibOpts::max_post_fit_sigma)
      .def_readwrite("max_spread_to_mid_pct", &CalibOpts::max_spread_to_mid_pct)
      .def_readwrite("per_slice_linear_fallback",
                     &CalibOpts::per_slice_linear_fallback);

  py::class_<SplineFitOpts>(m, "SplineFitOpts")
      .def(py::init<>())
      .def_readwrite("lambda_", &SplineFitOpts::lambda)
      .def_readwrite("mult_floor", &SplineFitOpts::mult_floor)
      .def_readwrite("mult_ceil", &SplineFitOpts::mult_ceil)
      .def_readwrite("min_obs", &SplineFitOpts::min_obs)
      .def_property_readonly(
          "grid",
          [](const SplineFitOpts &self) {
            return std::vector<double>{self.grid.begin(), self.grid.end()};
          },
          "The fixed standard moneyness grid (read-only: the C++ config borrows it).");

  py::class_<CurveConfig>(m, "CurveConfig")
      .def(py::init<>())
      .def_readwrite("kind", &CurveConfig::kind)
      .def_readwrite("convex", &CurveConfig::convex)
      .def_readwrite("parametric", &CurveConfig::parametric)
      .def_readwrite("spline", &CurveConfig::spline);

  py::class_<PricerConfig>(m, "PricerConfig")
      .def(py::init<>())
      .def_readwrite("preset", &PricerConfig::preset)
      .def_readwrite("n_threads", &PricerConfig::n_threads)
      .def_readwrite("fit_workers", &PricerConfig::fit_workers)
      .def_readwrite("collect_stage_timings", &PricerConfig::collect_stage_timings)
      .def_readwrite("cash_divs", &PricerConfig::cash_divs)
      .def_readwrite("query_pricing_tier", &PricerConfig::query_pricing_tier)
      .def_readwrite("curve", &PricerConfig::curve,
                     "Full optional CurveConfig. None selects profile routing.")
      .def_readwrite("use_correction_cache", &PricerConfig::use_correction_cache)
      .def_readwrite("score_parity", &PricerConfig::score_parity)
      .def_readwrite("enforce_calendar_floor", &PricerConfig::enforce_calendar_floor)
      .def_readwrite("use_deam_cache_for_fit", &PricerConfig::use_deam_cache_for_fit)
      .def_readwrite("audit_fit_inversions", &PricerConfig::audit_fit_inversions)
      .def_readwrite("warm_start_carry", &PricerConfig::warm_start_carry)
      .def_readwrite("max_obs_per_slice", &PricerConfig::max_obs_per_slice)
      .def_readwrite("max_deam_strikes_per_expiry",
                     &PricerConfig::max_deam_strikes_per_expiry)
      .def_readwrite("max_otm_shortcut_premium_spread_frac",
                     &PricerConfig::max_otm_shortcut_premium_spread_frac)
      // Backward-compatible convenience pin/unpin view over the full config.
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

  // Bound through `PyPricerFitter`, whose whole job is the reader/writer lock
  // the library's documented thread-safety contract needs and the GIL cannot
  // provide once `value_chain` releases it (see the class comment above).
  py::class_<PyPricerFitter>(m, "PricerFitter")
      .def(py::init<PricerConfig>(), py::arg("config") = PricerConfig{})
      .def("fit", &PyPricerFitter::fit, py::arg("chain"),
           "Fit the surface from `chain` and store it, replacing any prior fit.\n"
           "The prior surface is left intact on failure.\n\n"
           "Releases the GIL, and takes this fitter's WRITER lock while it does:\n"
           "`fit` mutates, so it is serialized against itself and against every\n"
           "concurrent `value_chain` / `surface` on the same object. Distinct\n"
           "fitters never contend.")
      .def_property_readonly("fitted", &PyPricerFitter::fitted)
      .def("set_threads", &PyPricerFitter::set_threads, py::arg("n"))
      .def("surface", &PyPricerFitter::surface,
           "The served surface for the config's default purpose.\n\n"
           "The returned handle CO-OWNS its generation: a later `fit()` publishes\n"
           "a new generation without invalidating a handle a caller still holds,\n"
           "and the handle stays valid after the fitter itself is dropped.")
      .def("value_chain", &PyPricerFitter::value_chain, py::arg("chain"), py::arg("fields"),
           py::arg("n_threads") = 0,
           "Price the chain's options for `fields` as numpy SoA columns.\n\n"
           "DETERMINISTIC: bit-identical for any `n_threads` (disjoint output\n"
           "slots, pure const reads). 0 => the config's n_threads; 1 => serial.\n"
           "Unrequested columns come back EMPTY, never zero-filled.\n\n"
           "Releases the GIL under this fitter's READER lock: concurrent\n"
           "`value_chain` calls on one fitter run together, as the library\n"
           "documents; a concurrent `fit` waits for them.")
      .def("value_chain_ids", &PyPricerFitter::value_chain_ids, py::arg("chain"),
           py::arg("ids"), py::arg("fields"), py::arg("n_threads") = 0,
           "Price only `ids`, preserving caller order and duplicates — the\n"
           "quote-update path, with work proportional to the selection.");
}
