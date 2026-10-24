// A curve session served through the live book (lane A1, r6 2026-10-23):
// api/analytics/served_session.hpp is the contract. Model/pricing access uses public APIs:
// VolaSession / PricedSurface for the cold route, ServeBook / ServeBookReader for the book.
#include "atx/vol/api/analytics/served_session.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "atx/vol/api/backtest/priced_surface.hpp"
#include "atx/vol/api/fitting/session.hpp"
#include "pricing/american_greek_availability.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::uint64_t kUnbound = std::numeric_limits<std::uint64_t>::max();
constexpr auto kSessionFields = GreekFieldMask::Price | GreekFieldMask::Delta |
    GreekFieldMask::Gamma | GreekFieldMask::Vega | GreekFieldMask::Theta;

[[nodiscard]] bool finite_positive(double x) noexcept { return std::isfinite(x) && x > 0.0; }

[[nodiscard]] bool valid_contract(double K, double T, Side side, double spot,
                                 std::int64_t now_ns) noexcept {
  return finite_positive(K) && finite_positive(T) && finite_positive(spot) && now_ns > 0 &&
         (side == Side::Call || side == Side::Put);
}

// Common output bits only. Record reasons keep their own vocabulary and axis
// order; no numerical-stencil conclusion is inferred from an edge-band refusal.
[[nodiscard]] GreekFieldMask book_support(AnalyticsInputClass input_class, double price,
    const std::array<double, 4> &values, const ServeGreekReasons &reasons) noexcept {
  if (!std::isfinite(price) || input_class == AnalyticsInputClass::Refused)
    return GreekFieldMask::None;
  auto fields = GreekFieldMask::Price;
  if (input_class == AnalyticsInputClass::PriceOnly) return fields;
  constexpr std::array<ServeGreek, 4> axes{
      ServeGreek::Delta, ServeGreek::Gamma, ServeGreek::Vega, ServeGreek::Theta};
  constexpr std::array<GreekFieldMask, 4> bits{
      GreekFieldMask::Delta, GreekFieldMask::Gamma, GreekFieldMask::Vega, GreekFieldMask::Theta};
  for (std::size_t i = 0; i < axes.size(); ++i) {
    const auto reason = reasons[static_cast<std::size_t>(axes[i])];
    if (std::isfinite(values[i]) &&
        (reason == ServeGreekReason::Served || reason == ServeGreekReason::Exercised))
      fields = fields | bits[i];
  }
  return fields;
}

[[nodiscard]] bool same_expiry(double a, double b) noexcept {
  return std::abs(a - b) <= kSessionBookTRelTol * std::max(1.0, std::abs(a));
}

[[nodiscard]] Result<std::vector<SessionBookExpiry>>
sorted_bindings(const std::shared_ptr<const ServeBook> &book,
                std::span<const SessionBookExpiry> expiries) {
  if (!book) return Err(ErrorCode::InvalidArgument, "ServedSession::bind: a null book");
  std::vector<SessionBookExpiry> out(expiries.begin(), expiries.end());
  for (const SessionBookExpiry &e : out) {
    if (!finite_positive(e.T)) {
      return Err(ErrorCode::InvalidArgument, "ServedSession::bind: a non-positive expiry T");
    }
  }
  std::sort(out.begin(), out.end(),
            [](const SessionBookExpiry &a, const SessionBookExpiry &b) { return a.T < b.T; });
  for (std::size_t i = 1; i < out.size(); ++i) {
    if (same_expiry(out[i - 1].T, out[i].T)) {
      return Err(ErrorCode::InvalidArgument, "ServedSession::bind: one expiry bound twice");
    }
  }
  return Ok(std::move(out));
}

struct CurveRead {
  double sigma = kNaN;
  double rate = kNaN;
  double yield = kNaN;
};

using CurveSurfacePtr = std::variant<const VolaSession *, const PricedSurface *>;

[[nodiscard]] CurveRead read_curve(const CurveSurfacePtr &s, double K, double T) noexcept {
  return std::visit(
      [K, T](const auto *surface) {
        return CurveRead{surface->iv(K, T), surface->rate_at(T), surface->q_eff_at(T)};
      },
      s);
}

} // namespace

Result<ServedSession> ServedSession::bind(const VolaSession &session,
                                          std::shared_ptr<const ServeBook> book,
                                          std::span<const SessionBookExpiry> expiries) {
  Result<std::vector<SessionBookExpiry>> sorted = sorted_bindings(book, expiries);
  if (!sorted) return Err(sorted.error());
  ServedSession out;
  out.surface_ = &session;
  out.cold_ = ColdRoute{session.inputs().S, session.inputs().deam.method,
                        session.inputs().deam.al_opts};
  out.book_ = std::move(book);
  out.bound_ = std::move(*sorted);
  return Ok(std::move(out));
}

Result<ServedSession> ServedSession::bind(const PricedSurface &surface,
                                          std::shared_ptr<const ServeBook> book,
                                          std::span<const SessionBookExpiry> expiries) {
  Result<std::vector<SessionBookExpiry>> sorted = sorted_bindings(book, expiries);
  if (!sorted) return Err(sorted.error());
  ServedSession out;
  out.surface_ = &surface;
  out.cold_ = ColdRoute{surface.pricing().S, surface.pricing().method, surface.pricing().al_opts};
  out.book_ = std::move(book);
  out.bound_ = std::move(*sorted);
  return Ok(std::move(out));
}

ServeBookReader ServedSession::reader() const { return book_->reader(); }

std::optional<std::uint64_t> ServedSession::bound_expiry(double T) const noexcept {
  const auto it = std::lower_bound(
      bound_.begin(), bound_.end(), T,
      [](const SessionBookExpiry &e, double t) { return e.T < t && !same_expiry(e.T, t); });
  if (it != bound_.end() && same_expiry(it->T, T)) return it->expiry_id;
  return std::nullopt;
}

// The session's own cold route at `spot`: the price is the session's american_price on its
// volatility, rate, yield, method and preset; the Greeks are american_greeks_fd on the same
// inputs (their map's price differs from it by price_map_delta, declared).
SessionServedAnswer ServedSession::cold(double K, double T, Side side, double spot,
                                        std::int64_t now_ns) const {
  SessionServedAnswer out;
  out.requested = kSessionFields;
  if (!valid_contract(K, T, side, spot, now_ns)) {
    out.status = AnalyticsStatus::InvalidArgument;
    return out;
  }
  out.declaration.model = AnalyticsModel::SessionCurve;
  out.declaration.convention = HedgeConvention::StickyStrike;
  out.declaration.clock.now_ns = now_ns;
  out.declaration.clock.tau = T;
  AmericanGreeks supplied{};
  detail::mark_american_greek_fields(supplied, kSessionFields, AmericanGreekReason::Nonfinite);
  out.cold_reasons = supplied.reasons;
  const CurveRead c = read_curve(surface_, K, T);
  const Result<double> price =
      american_price(spot, K, T, c.sigma, c.rate, c.yield, side, cold_.method, cold_.opts);
  if (!price || !std::isfinite(*price)) return out;
  const Result<AmericanGreeks> g =
      american_greeks_fd(spot, K, T, c.sigma, c.rate, c.yield, side, cold_.method, cold_.opts);
  out.price = *price;
  if (g) {
    supplied = *g;
    detail::narrow_american_greek_request(supplied, kSessionFields);
    out.delta = g->delta;
    out.gamma = g->gamma;
    out.vega = g->vega;
    out.theta = g->theta;
    out.declaration.price_map_delta = std::abs(g->price - *price);
  }
  // Price is supplied independently by the declared price map even when the FD
  // bundle fails. Its own finite value, not an unused default slot, backs this bit.
  supplied.price = *price;
  detail::mark_american_greek_fields(supplied, GreekFieldMask::Price,
                                   AmericanGreekReason::ComputedEstimate);
  out.supported = supplied.supported;
  out.cold_reasons = supplied.reasons;
  out.source = SessionServeSource::SessionCold;
  // A curve fit's cold route carries no error statement: an estimate, never certified.
  out.input_class = AnalyticsInputClass::Estimate;
  out.status = AnalyticsStatus::Estimate;
  return out;
}

SessionServedAnswer ServedSession::serve(ServeBookReader &reader, double K, double T, Side side,
                                         double spot, std::int64_t now_ns) const {
  if (!valid_contract(K, T, side, spot, now_ns)) {
    SessionServedAnswer bad;
    bad.requested = kSessionFields;
    bad.status = AnalyticsStatus::InvalidArgument;
    return bad;
  }
  const std::optional<std::uint64_t> id = bound_expiry(T);
  if (!id) return cold(K, T, side, spot, now_ns);
  ServeRequest q;
  q.strike = K;
  q.side = side;
  q.spot = spot;
  q.tier = ServeTier::Ladder;
  q.tier_max = ServeTier::Family;
  q.greeks = ServeGreekMask::four();
  q.now_ns = now_ns;
  q.family_bound = true;
  SessionServedAnswer out;
  out.requested = kSessionFields;
  out.book = reader.serve(*id, q);
  out.price = out.book.price;
  out.delta = out.book.delta;
  out.gamma = out.book.gamma;
  out.vega = out.book.vega;
  out.theta = out.book.theta;
  out.input_class = classify_served_answer(out.book);
  out.source = out.input_class == AnalyticsInputClass::Refused ? SessionServeSource::Refused
                                                             : SessionServeSource::Book;
  out.supported = book_support(out.input_class, out.price,
      {out.delta, out.gamma, out.vega, out.theta}, out.book.reason);
  switch (out.input_class) {
  case AnalyticsInputClass::Certified: out.status = AnalyticsStatus::Ok; break;
  case AnalyticsInputClass::Estimate: out.status = AnalyticsStatus::Estimate; break;
  case AnalyticsInputClass::PriceOnly: out.status = AnalyticsStatus::PriceOnly; break;
  case AnalyticsInputClass::Refused: out.status = AnalyticsStatus::Refused; break;
  }
  const ServeBookVersion &version = reader.pinned();
  AnalyticsDeclaration &d = out.declaration;
  d.book_version = version.number;
  d.expiry_id = *id;
  d.model = AnalyticsModel::QuotingRecord;
  d.greek_model = out.book.convention.model;
  d.convention = HedgeConvention::StickyStrike;
  d.clock.now_ns = now_ns;
  d.price_map_delta = out.book.price_map_delta;
  if (const ServeBookEntry *e = version.find(*id); e != nullptr) {
    d.entry_tag = e->tag;
    d.clock.expiry_ns = e->expiry_ns;
    d.clock.next_event_ns = e->next_event_ns;
    if (e->record) {
      d.record_fit_sha256 = e->record->version().fit_sha256;
      d.record_policy_sha256 = e->record->version().policy_sha256;
      d.record_input_sha256 = e->record->version().input_sha256;
      d.clock.as_of_ns = e->record->version().as_of_ns;
      d.clock.tau = e->record->tau();
    }
  }
  return out;
}

Status ServedSession::serve_chain(ServeBookReader &reader, double spot, std::int64_t now_ns,
                                  const SessionChainView &chain, const ChainTickOutputs &out,
                                  std::span<SessionChainStatement> statements,
                                  std::span<std::uint64_t> expiry_scratch) const {
  const std::size_t n = chain.size();
  if (!chain.consistent() || !out.consistent() || out.size() != n || statements.size() != n ||
      expiry_scratch.size() != n || !finite_positive(spot) || now_ns <= 0) {
    return Err(ErrorCode::InvalidArgument, "ServedSession::serve_chain: lengths, spot or clock");
  }
  for (std::size_t k = 0; k < n; ++k) {
    const std::optional<std::uint64_t> id = bound_expiry(chain.expiry[k]);
    expiry_scratch[k] = id ? *id : kUnbound;
  }
  // The book's contracts all come from ONE version: the reader's version is checked after
  // every one, and the chain is served again once when a publication lands mid-chain.
  for (int attempt = 0; attempt < 2; ++attempt) {
    std::optional<std::uint64_t> version;
    bool consistent = true;
    for (std::size_t k = 0; k < n; ++k) {
      SessionChainStatement &st = statements[k];
      st = SessionChainStatement{};
      if (expiry_scratch[k] == kUnbound) {
        const SessionServedAnswer a = cold(chain.strike[k], chain.expiry[k], chain.side[k], spot,
                                           now_ns);
        out.price[k] = a.price;
        out.delta[k] = a.delta;
        out.gamma[k] = a.gamma;
        out.vega[k] = a.vega;
        out.theta[k] = a.theta;
        out.status[k] = std::isfinite(a.price) ? TickStatus::Fallback : TickStatus::Unpriceable;
        st.source = a.source;
        st.input_class = a.input_class;
        st.requested = a.requested;
        st.supported = a.supported;
        st.cold_reasons = a.cold_reasons;
        continue;
      }
      const ServedChainView one{expiry_scratch.subspan(k, 1), chain.strike.subspan(k, 1),
                                chain.side.subspan(k, 1)};
      const ChainTickOutputs o{out.price.subspan(k, 1), out.delta.subspan(k, 1),
                               out.gamma.subspan(k, 1), out.vega.subspan(k, 1),
                               out.theta.subspan(k, 1), out.status.subspan(k, 1)};
      const Status served = reader.serve_chain(spot, now_ns, one, ServeTier::Ladder,
                                               ServeTier::Family, o,
                                               std::span<ServedChainStatement>(&st.book, 1));
      if (!served) return Err(served.error());
      st.input_class = classify_served_statement(st.book, out.price[k]);
      st.source = st.input_class == AnalyticsInputClass::Refused ? SessionServeSource::Refused
                                                               : SessionServeSource::Book;
      st.requested = kSessionFields;
      st.supported = book_support(st.input_class, out.price[k],
          {out.delta[k], out.gamma[k], out.vega[k], out.theta[k]}, st.book.greek_reasons);
      const std::uint64_t v = reader.pinned().number;
      if (version && *version != v) consistent = false;
      version = v;
    }
    if (consistent) return Ok();
  }
  return Err(ErrorCode::Unavailable, "ServedSession::serve_chain: the book published twice");
}

const char *to_string(SessionServeSource value) noexcept {
  switch (value) {
  case SessionServeSource::Book: return "book";
  case SessionServeSource::SessionCold: return "session_cold";
  case SessionServeSource::Refused: return "refused";
  }
  return "unknown";
}

} // namespace atx::vol
