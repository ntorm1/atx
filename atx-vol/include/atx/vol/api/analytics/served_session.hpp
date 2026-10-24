#pragma once

// A curve session served through the live book (lane A1, r6 2026-10-23; the session /
// PricedSurface / chain-tick wiring H1 deferred to A1, H1 report "Deviations" 3; review-H1
// "For A1"). docs/analytics-contracts.md section 6.
//
// A VolaSession (or its archived PricedSurface) serves curve fits; the live ServeBook
// (serve_book.hpp) serves quote-slice records. A ServedSession binds the two through their
// PUBLIC APIs only: the caller names which of the session's expiries the book holds
// (SessionBookExpiry: the session's T and the book's expiry id), and every query at a
// bound expiry is answered by the book's record (one ServeBookReader per reader thread,
// now_ns always supplied), every other query by the session's own cold route at the
// requested spot - american_greeks_fd on the session's volatility, rate, yield, method
// and preset: price and Greeks from one map, the price bit-identical to the session's
// cold american_price. Each answer says which model answered (AnalyticsModel
// QuotingRecord / SessionCurve), with the book's statement verbatim on a book answer.
// requested/supported separately describe Price/Delta/Gamma/Vega/Theta availability;
// route and model/price statuses do not assert that every Greek is supplied. Cold
// numerical reasons and book reasons retain their different axis vocabularies.
//
// The chain route (serve_chain) feeds chain_tick's ChainTickOutputs: runs of contracts at
// bound expiries go through ServeBookReader::serve_chain (one version for the whole
// chain: the reader's version is checked after every run, the chain is redone once when
// a publication lands mid-chain, and a second move is an Unavailable error), with each
// contract's ServedChainStatement; the other contracts take the session's cold route,
// TickStatus::Fallback (an answer of another model, covered by no certificate). A Sampled
// book answer stays TickStatus::Fallback (H1's rule): Ok is never a certificate claim.
//
// Not a session mutation: session.cpp is not modified; the binding borrows the session
// (it must outlive the binding) and owns a shared reference to the book.
//
// Threads: the binding is immutable after bind(); serve / serve_chain are const and take
// the caller's reader, so one binding serves any number of reader threads, one reader
// each. Cost: a bound query is the book's (allocation-free); an unbound one is the cold
// FD Greek bundle (several Andersen-Lake solves).

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "atx/vol/api/analytics/served_analytics.hpp"
#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/fitting/serve_book.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/chain_tick.hpp"

namespace atx::vol {

class VolaSession;
class PricedSurface;

// One of the session's expiries the book holds.
struct SessionBookExpiry {
  double T = 0.0;              // the session's year fraction for that expiry
  std::uint64_t expiry_id = 0; // the book's key
};
// Two T's are the same expiry when |T_a - T_b| <= kSessionBookTRelTol x max(1, T_a).
inline constexpr double kSessionBookTRelTol = 1.0e-12;

enum class SessionServeSource : std::uint8_t {
  Book = 0,        // the record of a bound expiry answered (see the statement)
  SessionCold = 1, // the session's cold route answered (unbound expiry)
  Refused = 2,     // neither produced a number
};

struct SessionServedAnswer {
  double price = std::numeric_limits<double>::quiet_NaN();
  double delta = std::numeric_limits<double>::quiet_NaN();
  double gamma = std::numeric_limits<double>::quiet_NaN();
  double vega = std::numeric_limits<double>::quiet_NaN();
  double theta = std::numeric_limits<double>::quiet_NaN();
  SessionServeSource source = SessionServeSource::Refused;
  ServeAnswer book{}; // the record's answer verbatim (source Book)
  AnalyticsInputClass input_class = AnalyticsInputClass::Refused;
  AnalyticsStatus status = AnalyticsStatus::Refused;
  AnalyticsDeclaration declaration{};
  // Availability of this answer's five output axes, separate from its price
  // certificate/model status. Common bits are Price/Delta/Gamma/Vega/Theta only.
  GreekFieldMask requested = GreekFieldMask::None;
  GreekFieldMask supported = GreekFieldMask::None;
  // Cold FD reasons in AmericanGreeks' nine-axis order. A book answer leaves
  // these NotRequested and carries its distinct six-axis reasons in book.reason.
  // Failed cold attempts may carry Nonfinite; no refusal is an accuracy claim.
  std::array<AmericanGreekReason, 9> cold_reasons{};
};

struct SessionChainView {
  std::span<const double> strike;
  std::span<const double> expiry; // the session's year fractions
  std::span<const Side> side;
  [[nodiscard]] std::size_t size() const noexcept { return strike.size(); }
  [[nodiscard]] bool consistent() const noexcept {
    return expiry.size() == strike.size() && side.size() == strike.size();
  }
};

struct SessionChainStatement {
  ServedChainStatement book{}; // H1's statement (source Book; default otherwise)
  SessionServeSource source = SessionServeSource::Refused;
  AnalyticsInputClass input_class = AnalyticsInputClass::Refused;
  GreekFieldMask requested = GreekFieldMask::None;
  GreekFieldMask supported = GreekFieldMask::None;
  // Same provenance split as SessionServedAnswer; book.greek_reasons retains
  // the record vocabulary, including Phi, without mapping it onto Vanna.
  std::array<AmericanGreekReason, 9> cold_reasons{};
};

class ServedSession {
public:
  // InvalidArgument for a null book, a non-finite / non-positive T or two bindings of one
  // T. The session / surface is borrowed and must outlive the binding.
  [[nodiscard]] static Result<ServedSession> bind(const VolaSession &session,
                                                  std::shared_ptr<const ServeBook> book,
                                                  std::span<const SessionBookExpiry> expiries);
  [[nodiscard]] static Result<ServedSession> bind(const PricedSurface &surface,
                                                  std::shared_ptr<const ServeBook> book,
                                                  std::span<const SessionBookExpiry> expiries);

  // A reader for the calling thread (one per thread).
  [[nodiscard]] ServeBookReader reader() const;

  // (K, T, side) at `spot`: the book at a bound T (tiers Ladder..Family, Greeks four,
  // family_bound true), else the session's cold route. InvalidArgument status (no number)
  // for a non-finite / non-positive K, T or spot, or now_ns <= 0.
  [[nodiscard]] SessionServedAnswer serve(ServeBookReader &reader, double K, double T, Side side,
                                          double spot, std::int64_t now_ns) const;

  // Every contract of `chain` at `spot` into `out` and `statements` (file header).
  // `expiry_scratch` holds one id per contract (caller-owned, reused per tick).
  // @return InvalidArgument for mismatched lengths, a bad spot or now_ns <= 0 (nothing
  //         written); Unavailable when the book published twice during the chain.
  [[nodiscard]] Status serve_chain(ServeBookReader &reader, double spot, std::int64_t now_ns,
                                   const SessionChainView &chain, const ChainTickOutputs &out,
                                   std::span<SessionChainStatement> statements,
                                   std::span<std::uint64_t> expiry_scratch) const;

  // The book expiry bound to T, or nullopt.
  [[nodiscard]] std::optional<std::uint64_t> bound_expiry(double T) const noexcept;
  [[nodiscard]] std::span<const SessionBookExpiry> bindings() const noexcept { return bound_; }

private:
  struct ColdRoute {
    double spot_ref = 0.0;
    AmericanMethod method = AmericanMethod::AndersenLake;
    std::optional<AlOpts> opts = std::nullopt;
  };
  ServedSession() = default;
  [[nodiscard]] SessionServedAnswer cold(double K, double T, Side side, double spot,
                                         std::int64_t now_ns) const;
  std::variant<const VolaSession *, const PricedSurface *> surface_{};
  ColdRoute cold_{};
  std::shared_ptr<const ServeBook> book_;
  std::vector<SessionBookExpiry> bound_; // ascending T
};

[[nodiscard]] const char *to_string(SessionServeSource value) noexcept;

} // namespace atx::vol
