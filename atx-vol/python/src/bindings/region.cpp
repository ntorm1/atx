// Exercise-region classification (lane C1, pricing/american_region.hpp) and the D1
// schedule-grouped discrete-dividend batch (pricing/american_discrete_div_batch.hpp), as
// numpy batches in pricing.cpp's style (lane PY, 2026-10-23).
//
// BATCH CONTRACT (batch_status.hpp): equal-length 1-D columns; a bare Side broadcasts.
// A per-lane refusal is a value in the lane's typed outcome column (AlExerciseClass,
// DiscreteDivBatchOutcome) or a NaN with an int(ErrorCode) status, never an exception;
// an exception means the CALL was malformed. The loops release the GIL.
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_discrete_div_batch.hpp"
#include "atx/vol/api/pricing/american_discrete_div_kernel.hpp"
#include "atx/vol/api/pricing/american_region.hpp"
#include "batch_status.hpp"
#include "common.hpp"
#include "result.hpp"

namespace py = pybind11;
using namespace atx::vol;
using atxvol::python::F64Column;

namespace {

constexpr double kNan = std::numeric_limits<double>::quiet_NaN();

template <class T> py::array_t<T> new_array(std::size_t n) {
  return py::array_t<T>(static_cast<py::ssize_t>(n));
}

// ── exercise region ─────────────────────────────────────────────────────────────────

py::tuple classified_batch(const F64Column &S, const F64Column &K, const F64Column &T,
                           const F64Column &sigma, const F64Column &r, const F64Column &q,
                           const py::object &side, AlExerciseRegion rule,
                           const std::optional<AlOpts> &opts) {
  const std::size_t n = static_cast<std::size_t>(S.size());
  const auto s = atxvol::python::column(S, "S", n, "S");
  const auto k = atxvol::python::column(K, "K", n, "S");
  const auto t = atxvol::python::column(T, "T", n, "S");
  const auto v = atxvol::python::column(sigma, "sigma", n, "S");
  const auto rr = atxvol::python::column(r, "r", n, "S");
  const auto qq = atxvol::python::column(q, "q", n, "S");
  const std::vector<Side> sides = atxvol::python::side_column(side, n, "S");
  auto price = new_array<double>(n);
  auto boundary = new_array<double>(n);
  auto cls = new_array<std::uint8_t>(n);
  auto status = new_array<std::int32_t>(n);
  double *p = price.mutable_data();
  double *b = boundary.mutable_data();
  std::uint8_t *c = cls.mutable_data();
  std::int32_t *st = status.mutable_data();
  {
    py::gil_scoped_release release;
    for (std::size_t i = 0; i < n; ++i) {
      const auto res = andersen_lake_classified(s[i], k[i], t[i], v[i], rr[i], qq[i], sides[i],
                                                opts, rule);
      if (!res) {
        p[i] = kNan;
        b[i] = kNan;
        c[i] = static_cast<std::uint8_t>(AlExerciseClass::UnsupportedRegime);
        st[i] = static_cast<std::int32_t>(res.error().code());
        continue;
      }
      p[i] = res->price;
      b[i] = res->boundary;
      c[i] = static_cast<std::uint8_t>(res->cls);
      st[i] = atxvol::python::kStatusOk;
    }
  }
  return py::make_tuple(std::move(price), std::move(boundary), std::move(cls), std::move(status));
}

py::array_t<std::uint8_t> classify_batch(const F64Column &S, const F64Column &K,
                                         const F64Column &T, const F64Column &r,
                                         const F64Column &q, const F64Column &sigma,
                                         const py::object &side, const F64Column &boundary) {
  const std::size_t n = static_cast<std::size_t>(S.size());
  const auto s = atxvol::python::column(S, "S", n, "S");
  const auto k = atxvol::python::column(K, "K", n, "S");
  const auto t = atxvol::python::column(T, "T", n, "S");
  const auto rr = atxvol::python::column(r, "r", n, "S");
  const auto qq = atxvol::python::column(q, "q", n, "S");
  const auto v = atxvol::python::column(sigma, "sigma", n, "S");
  const auto bd = atxvol::python::column(boundary, "boundary", n, "S");
  const std::vector<Side> sides = atxvol::python::side_column(side, n, "S");
  auto out = new_array<std::uint8_t>(n);
  std::uint8_t *o = out.mutable_data();
  {
    py::gil_scoped_release release;
    for (std::size_t i = 0; i < n; ++i) {
      o[i] = static_cast<std::uint8_t>(classify_exercise_region(
          s[i], k[i], t[i], rr[i], qq[i], v[i], sides[i] == Side::Call, bd[i]));
    }
  }
  return out;
}

// ── D1 discrete-dividend batch ──────────────────────────────────────────────────────

using DividendEvents = std::vector<std::pair<double, double>>;

// The batch borrows each group's schedule (NON-OWNING spans): this wrapper owns them for
// the batch's whole life, and re-preparing replaces both together.
class PyDiscreteDivBatch {
public:
  PyDiscreteDivBatch(const DiscreteDivProductionOpts &opts, int screen_steps)
      : batch_(opts, screen_steps) {}

  std::size_t prepare(const std::vector<std::tuple<double, double, double, double, DividendEvents>> &groups) {
    std::vector<std::vector<CashDividend>> schedules;
    schedules.reserve(groups.size());
    for (const auto &g : groups) {
      std::vector<CashDividend> events;
      for (const auto &[tau, amount] : std::get<4>(g)) events.push_back(CashDividend{tau, amount});
      schedules.push_back(std::move(events));
    }
    std::vector<DiscreteDivBatchGroup> specs;
    specs.reserve(groups.size());
    for (std::size_t i = 0; i < groups.size(); ++i) {
      const auto &g = groups[i];
      specs.push_back(DiscreteDivBatchGroup{std::get<0>(g), std::get<1>(g), std::get<2>(g),
                                            std::get<3>(g), schedules[i]});
    }
    std::size_t refused = 0;
    {
      py::gil_scoped_release release;
      refused = batch_.prepare(specs);
    }
    // The vectors' heap buffers do not move when the outer vector is moved in, so the
    // spans handed to prepare() stay valid.
    schedules_ = std::move(schedules);
    return refused;
  }

  [[nodiscard]] std::size_t groups() const noexcept { return batch_.groups(); }
  [[nodiscard]] bool group_ok(std::size_t g) const noexcept { return batch_.group_ok(g); }

  [[nodiscard]] py::dict route(std::size_t g) const {
    if (g >= batch_.groups()) throw py::index_error("group index out of range");
    const DiscreteDivLatticeRoute &r = batch_.route(g);
    py::dict d;
    d["method"] = r.method;
    d["steps"] = r.steps;
    d["coarse_steps"] = r.coarse_steps;
    d["pre_steps"] = r.pre_steps;
    d["events"] = r.events;
    d["first_ex_step"] = r.first_ex_step;
    d["max_cash_ratio"] = r.max_cash_ratio;
    d["grid_bottom_depth"] = r.grid_bottom_depth;
    d["flags"] = r.flags;
    return d;
  }

  std::vector<DiscreteDivBatchContract> contracts(const py::array_t<std::uint32_t, py::array::c_style | py::array::forcecast> &group,
                                                  const F64Column &K, const py::object &side,
                                                  const F64Column &value, bool is_quote) const {
    if (group.ndim() != 1) throw py::value_error("group must be a one-dimensional array");
    const std::size_t n = static_cast<std::size_t>(group.size());
    const auto k = atxvol::python::column(K, "K", n, "group");
    const auto v = atxvol::python::column(value, is_quote ? "quote" : "sigma", n, "group");
    const std::vector<Side> sides = atxvol::python::side_column(side, n, "group");
    std::vector<DiscreteDivBatchContract> out(n);
    const std::uint32_t *g = group.data();
    for (std::size_t i = 0; i < n; ++i) {
      out[i].group = g[i];
      out[i].K = k[i];
      out[i].side = sides[i];
      if (is_quote) {
        out[i].quote = v[i];
      } else {
        out[i].sigma = v[i];
      }
    }
    return out;
  }

  static py::tuple results(const std::vector<DiscreteDivBatchResult> &res, bool iv) {
    const std::size_t n = res.size();
    auto value = new_array<double>(n);
    auto outcome = new_array<std::uint8_t>(n);
    double *pv = value.mutable_data();
    std::uint8_t *po = outcome.mutable_data();
    for (std::size_t i = 0; i < n; ++i) {
      pv[i] = res[i].value;
      po[i] = static_cast<std::uint8_t>(res[i].outcome);
    }
    if (!iv) return py::make_tuple(std::move(value), std::move(outcome));
    auto fallback = new_array<std::uint8_t>(n);
    auto screen = new_array<std::uint16_t>(n);
    auto accurate = new_array<std::uint16_t>(n);
    std::uint8_t *pf = fallback.mutable_data();
    std::uint16_t *ps = screen.mutable_data();
    std::uint16_t *pa = accurate.mutable_data();
    for (std::size_t i = 0; i < n; ++i) {
      pf[i] = static_cast<std::uint8_t>(res[i].fallback);
      ps[i] = res[i].screen_lattices;
      pa[i] = res[i].accurate_lattices;
    }
    return py::make_tuple(std::move(value), std::move(outcome), std::move(fallback),
                          std::move(screen), std::move(accurate));
  }

  DiscreteDivBatch batch_;

private:
  std::vector<std::vector<CashDividend>> schedules_;
};

using GroupColumn = py::array_t<std::uint32_t, py::array::c_style | py::array::forcecast>;

void bind_enums(py::module_ &m) {
  py::enum_<AlExerciseRegion>(m, "AlExerciseRegion")
      .value("KERNEL_FLOOR", AlExerciseRegion::KernelFloor)
      .value("INTRINSIC", AlExerciseRegion::Intrinsic);
  py::enum_<AlExerciseClass>(m, "AlExerciseClass")
      .value("CONTINUATION", AlExerciseClass::Continuation)
      .value("EXERCISE_PLATEAU", AlExerciseClass::ExercisePlateau)
      .value("EUROPEAN", AlExerciseClass::European)
      .value("EXPIRY_LIMIT", AlExerciseClass::ExpiryLimit)
      .value("ZERO_VOL_LIMIT", AlExerciseClass::ZeroVolLimit)
      .value("UNSUPPORTED_REGIME", AlExerciseClass::UnsupportedRegime);
  atxvol::python::register_enum_names<AlExerciseClass>("AlExerciseClass");

  py::enum_<DiscreteDivBatchOutcome>(m, "DiscreteDivBatchOutcome")
      .value("PRICED", DiscreteDivBatchOutcome::Priced)
      .value("PRICE_ONLY", DiscreteDivBatchOutcome::PriceOnly)
      .value("REFUSED_INPUT", DiscreteDivBatchOutcome::RefusedInput)
      .value("REFUSED_RANGE", DiscreteDivBatchOutcome::RefusedRange)
      .value("REFUSED_BUDGET", DiscreteDivBatchOutcome::RefusedBudget)
      .value("REFUSED_GROUP", DiscreteDivBatchOutcome::RefusedGroup);
  atxvol::python::register_enum_names<DiscreteDivBatchOutcome>("DiscreteDivBatchOutcome");
  py::enum_<DiscreteDivIvFallback>(m, "DiscreteDivIvFallback")
      .value("NONE", DiscreteDivIvFallback::None)
      .value("NOT_APPLICABLE", DiscreteDivIvFallback::NotApplicable)
      .value("RANGE_EDGE", DiscreteDivIvFallback::RangeEdge)
      .value("NOT_INCREASING", DiscreteDivIvFallback::NotIncreasing)
      .value("VOL_BOUND", DiscreteDivIvFallback::VolBound)
      .value("PRICER_FAILURE", DiscreteDivIvFallback::PricerFailure)
      .value("NO_CONVERGENCE", DiscreteDivIvFallback::NoConvergence);
  atxvol::python::register_enum_names<DiscreteDivIvFallback>("DiscreteDivIvFallback");
  py::enum_<DiscreteDivAccuracy>(m, "DiscreteDivAccuracy")
      .value("STANDARD", DiscreteDivAccuracy::Standard)
      .value("REFINED", DiscreteDivAccuracy::Refined);
  py::enum_<DiscreteDivMethod>(m, "DiscreteDivMethod")
      .value("HISTORICAL_SPLICED", DiscreteDivMethod::HistoricalSpliced)
      .value("CUBIC_CRR", DiscreteDivMethod::CubicCrr)
      .value("CUBIC_BBS_RICHARDSON", DiscreteDivMethod::CubicBbsRichardson);
  atxvol::python::register_enum_names<DiscreteDivMethod>("DiscreteDivMethod");
  py::enum_<DiscreteDivTier>(m, "DiscreteDivTier")
      .value("ACCURATE", DiscreteDivTier::Accurate)
      .value("SCREEN", DiscreteDivTier::Screen);
}

void bind_batch(py::module_ &m) {
  py::class_<PyDiscreteDivBatch>(
      m, "DiscreteDivBatch",
      "D1's schedule-grouped discrete-dividend batch: prepare() the groups (one per "
      "(S, T, r, q, schedule)), then price / implied_vol / greeks numpy columns tagged "
      "by group index. Per-contract outcomes are typed (DiscreteDivBatchOutcome).")
      .def(py::init([](DiscreteDivAccuracy accuracy, int base_steps, bool kill_switch,
                       int screen_steps) {
             DiscreteDivProductionOpts opts;
             opts.accuracy = accuracy;
             opts.base_steps = base_steps;
             opts.kill_switch = kill_switch;
             return PyDiscreteDivBatch{opts, screen_steps};
           }),
           py::arg("accuracy") = DiscreteDivAccuracy::Standard,
           py::arg("base_steps") = kDiscreteDivDefaultSteps, py::arg("kill_switch") = false,
           py::arg("screen_steps") = kDiscreteDivScreenSteps)
      .def("prepare", &PyDiscreteDivBatch::prepare, py::arg("groups"),
           "groups: [(S, T, r, q, [(tau, amount), ...]), ...]; returns the refused count.")
      .def_property_readonly("groups", &PyDiscreteDivBatch::groups)
      .def("group_ok", &PyDiscreteDivBatch::group_ok, py::arg("group"))
      .def("route", &PyDiscreteDivBatch::route, py::arg("group"))
      .def(
          "price",
          [](PyDiscreteDivBatch &self, const GroupColumn &group, const F64Column &K,
             const py::object &side, const F64Column &sigma, ExerciseStyle exercise) {
            const auto contracts = self.contracts(group, K, side, sigma, false);
            std::vector<DiscreteDivBatchResult> out(contracts.size());
            Status status;
            {
              py::gil_scoped_release release;
              status = self.batch_.price(contracts, exercise, out);
            }
            atxvol::python::unwrap(status);
            return PyDiscreteDivBatch::results(out, false);
          },
          py::arg("group"), py::arg("K"), py::arg("side"), py::arg("sigma"),
          py::arg("exercise") = ExerciseStyle::American,
          "-> (price float64[n], outcome uint8[n] = DiscreteDivBatchOutcome)")
      .def(
          "implied_vol",
          [](PyDiscreteDivBatch &self, const GroupColumn &group, const F64Column &K,
             const py::object &side, const F64Column &quote, double tol, std::uint16_t max_iter,
             DiscreteDivTier tier, int screen_steps, double seed_sigma) {
            const auto contracts = self.contracts(group, K, side, quote, true);
            DiscreteDivIvOpts iv;
            iv.tol = tol;
            iv.max_iter = max_iter;
            iv.tier = tier;
            iv.screen_steps = screen_steps;
            iv.seed_sigma = seed_sigma;
            std::vector<DiscreteDivBatchResult> out(contracts.size());
            Status status;
            {
              py::gil_scoped_release release;
              status = self.batch_.implied_vol(contracts, iv, out);
            }
            atxvol::python::unwrap(status);
            return PyDiscreteDivBatch::results(out, true);
          },
          py::arg("group"), py::arg("K"), py::arg("side"), py::arg("quote"),
          py::arg("tol") = 1.0e-7, py::arg("max_iter") = 64,
          py::arg("tier") = DiscreteDivTier::Screen,
          py::arg("screen_steps") = kDiscreteDivScreenSteps, py::arg("seed_sigma") = 0.0,
          "-> (sigma, outcome, fallback uint8, screen_lattices uint16, accurate_lattices uint16)")
      .def(
          "greeks",
          [](PyDiscreteDivBatch &self, const GroupColumn &group, const F64Column &K,
             const py::object &side, const F64Column &sigma, ExerciseStyle exercise,
             double roll_horizon) {
            const auto contracts = self.contracts(group, K, side, sigma, false);
            std::vector<DiscreteDivProductionGreeks> out(contracts.size());
            Status status;
            {
              py::gil_scoped_release release;
              status = self.batch_.greeks(contracts, exercise, roll_horizon, out);
            }
            atxvol::python::unwrap(status);
            const std::size_t n = out.size();
            py::dict d;
            const auto column_of = [&](auto field) {
              auto a = new_array<double>(n);
              double *p = a.mutable_data();
              for (std::size_t i = 0; i < n; ++i) p[i] = out[i].*field;
              return a;
            };
            d["price"] = column_of(&DiscreteDivProductionGreeks::price);
            d["delta"] = column_of(&DiscreteDivProductionGreeks::delta);
            d["gamma"] = column_of(&DiscreteDivProductionGreeks::gamma);
            d["vega"] = column_of(&DiscreteDivProductionGreeks::vega);
            d["rho"] = column_of(&DiscreteDivProductionGreeks::rho);
            d["phi"] = column_of(&DiscreteDivProductionGreeks::phi);
            d["theta"] = column_of(&DiscreteDivProductionGreeks::theta);
            d["charm"] = column_of(&DiscreteDivProductionGreeks::charm);
            auto unsupported = new_array<std::uint16_t>(n);
            auto reasons = new_array<std::uint16_t>(n);
            std::uint16_t *pu = unsupported.mutable_data();
            std::uint16_t *pr = reasons.mutable_data();
            for (std::size_t i = 0; i < n; ++i) {
              pu[i] = out[i].unsupported;
              pr[i] = out[i].reasons;
            }
            // Bit masks: 1 << DiscreteDivGreek / 1 << DiscreteDivUnsupportedReason
            // (american_discrete_div_kernel.hpp); an unsupported Greek's value is NaN.
            d["unsupported"] = std::move(unsupported);
            d["reasons"] = std::move(reasons);
            return d;
          },
          py::arg("group"), py::arg("K"), py::arg("side"), py::arg("sigma"),
          py::arg("exercise") = ExerciseStyle::American, py::arg("roll_horizon") = 0.0);
}

} // namespace

void bind_region(py::module_ &m) {
  bind_enums(m);
  m.attr("AL_EXPIRY_LIMIT_T") = kAlExpiryLimitT;
  m.attr("AL_ZERO_VOL_SIGMA") = kAlZeroVolSigma;
  m.def(
      "classify_exercise_region",
      [](double S, double K, double T, double r, double q, double sigma, Side side,
         double boundary) {
        return classify_exercise_region(S, K, T, r, q, sigma, side == Side::Call, boundary);
      },
      py::arg("S"), py::arg("K"), py::arg("T"), py::arg("r"), py::arg("q"), py::arg("sigma"),
      py::arg("side"), py::arg("boundary"),
      "The Andersen-Lake exercise class of one contract against the internal put's B(T).");
  m.def("classify_exercise_region_batch", &classify_batch, py::arg("S"), py::arg("K"),
        py::arg("T"), py::arg("r"), py::arg("q"), py::arg("sigma"), py::arg("side"),
        py::arg("boundary"), "-> uint8[n] of AlExerciseClass values.");
  m.def(
      "andersen_lake_classified",
      [](double S, double K, double T, double sigma, double r, double q, Side side,
         AlExerciseRegion rule, const std::optional<AlOpts> &opts) {
        const AlRegionPrice p =
            atxvol::python::unwrap(andersen_lake_classified(S, K, T, sigma, r, q, side, opts, rule));
        return py::make_tuple(p.price, p.boundary, p.cls);
      },
      py::arg("S"), py::arg("K"), py::arg("T"), py::arg("sigma"), py::arg("r"), py::arg("q"),
      py::arg("side"), py::arg("rule") = AlExerciseRegion::KernelFloor,
      py::arg("opts") = std::nullopt,
      "-> (price, boundary, AlExerciseClass); UnsupportedRegime is a typed NaN price, other "
      "errors raise AtxError.");
  m.def("andersen_lake_classified_batch", &classified_batch, py::arg("S"), py::arg("K"),
        py::arg("T"), py::arg("sigma"), py::arg("r"), py::arg("q"), py::arg("side"),
        py::arg("rule") = AlExerciseRegion::KernelFloor, py::arg("opts") = std::nullopt,
        "-> (price f8[n], boundary f8[n], class u1[n], status i4[n]); NaN + int(ErrorCode) "
        "status for a lane the kernel refuses (STATUS_OK otherwise).");
  bind_batch(m);
}
