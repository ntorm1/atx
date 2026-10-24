// risk_api_check — compiles every public header of the risk engine (and the
// SpiderRock store header) in one translation unit so the FROZEN API contract of
// the 2026-10-24 sprint is type-checked on the base before any lane implements it,
// and so an ODR / include-order drift between the headers fails the build here
// rather than in a lane. Deliberately no definitions: this TU must stay empty.

#include "atx/vol/api/marketdata/spiderrock_store.hpp"
#include "atx/vol/api/risk/historical_var.hpp"
#include "atx/vol/api/risk/market_state.hpp"
#include "atx/vol/api/risk/pnl_explain.hpp"
#include "atx/vol/api/risk/risk_book.hpp"
#include "atx/vol/api/risk/risk_frame.hpp"
#include "atx/vol/api/risk/risk_kernel.hpp"
#include "atx/vol/api/risk/risk_types.hpp"
#include "atx/vol/api/risk/strike_coord.hpp"
#include "atx/vol/api/risk/theo_contract.hpp"

namespace atx::vol {
namespace {
// Aggregate-shape pins: a lane that changes a frozen aggregate's field count must
// come back to the base (risk_types.hpp banner).
static_assert(sizeof(OptionLeg) > 0);
static_assert(sizeof(ShareLeg) == 24);
static_assert(sizeof(StrikeCoord) == 24);
} // namespace
} // namespace atx::vol
