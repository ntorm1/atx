#pragma once

// Test seam for fit_quote_surface's publication enforcement (lane C2 fix round 1, review m1).
//
// Under QuoteSurfacePublication::Validate the surface enforces what the publication check
// finds in two places:
// - after a mutation, it rolls the repair trial back (publication_rollbacks);
// - in the final pass, it refuses a slice ("publication_invalid") or refuses and withholds
//   a row.
//
// No fit reaches either today. Every mutation path already re-certifies what the
// validator checks: the certificate, the calendar against every accepted slice, and the
// non-dividend certificate under Validate. The market-distance gate also removes the
// QuoteBox rows first. So the enforcement is a backstop.
//
// This seam (the expiry_schedule.hpp pattern) lets a test arm an injection on the calling
// thread. The check then reports a finding, and the test proves that the enforcement acts
// and that a revert of it is caught. Production never arms it: it costs one thread_local
// load per check, under Validate only.

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>

#include "atx/vol/api/fitting/quote_slice.hpp"

namespace atx::vol::detail {

struct PublicationInjection {
  static constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();
  std::size_t rollback_expiry = kNone; // every after-mutation check of this expiry fails
  std::size_t refuse_expiry = kNone;   // the final pass finds one Shape violation here, once
  std::size_t withhold_expiry = kNone; // the final pass finds one QuoteBox violation ...
  std::size_t row = 0;                 // ... at this row of withhold_expiry, once
  std::size_t injected = 0;            // findings injected so far
};

inline thread_local PublicationInjection *t_publication_injection = nullptr;

// After-mutation check of a trial for `expiry`: true when an armed injection fails it.
[[nodiscard]] inline bool inject_rollback(std::size_t expiry) noexcept {
  PublicationInjection *p = t_publication_injection;
  if (p == nullptr || p->rollback_expiry != expiry) return false;
  ++p->injected;
  return true;
}

// Final pass: adds each armed finding once to `report`, whose slices are the expiries `set`.
inline void inject_final(QuoteSlicePublicationReport &report, std::span<const std::size_t> set) {
  PublicationInjection *p = t_publication_injection;
  if (p == nullptr) return;
  const auto add = [&](std::size_t &expiry, QuoteSlicePublicationViolation kind,
                       std::size_t row) {
    const auto at = std::find(set.begin(), set.end(), expiry);
    if (expiry == PublicationInjection::kNone || at == set.end()) return;
    QuoteSlicePublicationFinding finding;
    finding.kind = kind;
    finding.expiry = static_cast<std::size_t>(at - set.begin());
    finding.row = row;
    report.findings.push_back(finding);
    ++report.violations[static_cast<std::size_t>(kind)];
    ++p->injected;
    expiry = PublicationInjection::kNone;
  };
  add(p->refuse_expiry, QuoteSlicePublicationViolation::Shape, kQuoteSlicePublicationAll);
  add(p->withhold_expiry, QuoteSlicePublicationViolation::QuoteBox, p->row);
}

// RAII arm and restore of the calling thread's injection (tests only).
class ScopedPublicationInjection {
public:
  explicit ScopedPublicationInjection(PublicationInjection &injection) noexcept
      : prev_(t_publication_injection) {
    t_publication_injection = &injection;
  }
  ~ScopedPublicationInjection() { t_publication_injection = prev_; }
  ScopedPublicationInjection(const ScopedPublicationInjection &) = delete;
  ScopedPublicationInjection &operator=(const ScopedPublicationInjection &) = delete;

private:
  PublicationInjection *prev_;
};

} // namespace atx::vol::detail
