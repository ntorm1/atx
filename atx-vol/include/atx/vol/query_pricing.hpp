#pragma once

#include <cstdint>

namespace atx::vol {

// Explicit query-time accuracy/latency contract. The tier never changes a
// fitted curve or its admission result; it controls only how an already-fitted
// European-equivalent volatility is re-Americanized for marks and Greeks.
// LegacyCompatible preserves the historical behavior of the serving object:
// live sessions may use their legacy cache, while archived PricedSurface values
// remain cold unless a fast tier is requested explicitly.
enum class QueryPricingTier : std::uint8_t {
  LegacyCompatible = 0,
  ColdReference = 1,
  RepresentativeFast = 2,
  CarryBank = 3,
};

// Per-call execution override. Configured follows the surface's prepared query
// tier; ColdReference bypasses any transient correction accelerator without
// rebuilding or mutating the immutable surface.
enum class QueryExecution : std::uint8_t {
  Configured = 0,
  ColdReference = 1,
};

// Per-query route introspection. A fast-configured surface reports
// ColdFallback when the resolved point is outside its certified correction box.
enum class QueryPricingRoute : std::uint8_t {
  ColdReference = 0,
  RepresentativeFast = 1,
  CarryBank = 2,
  ColdFallback = 3,
};

} // namespace atx::vol
