// instrumentation_abi.cpp — the link-time half of the build-configuration ODR
// guard for the two opt-in instrumentation headers (plan 5.2).
//
// `ATX_VOL_COUNTERS` and `ATX_VOL_PROFILE` change the DEFINITION of inline
// entities in atx/vol/detail/counters.hpp and atx/vol/detail/phase_profile.hpp.
// Those headers put the configuration in an inline namespace name, so a TU and a
// library that disagree declare DIFFERENT entities rather than colliding — and
// each header's cold entry points (`snapshot()`, `reset()`) call the one symbol
// defined below, which exists only under the tag its TU was compiled with.
//
// This TU is part of atx-vol, so it always carries the LIBRARY's view of both
// options. A consumer compiled with a different view therefore asks the linker
// for `atx::vol::counters::counters_off::detail::
// assert_build_configuration_matches()` while the library provides the
// `counters_on` one, and the link fails naming both. That is the entire point:
// the mismatch used to be undefined behaviour with no diagnostic at all.
//
// The tag is spelled out here rather than reused from the header's macro, which
// the header `#undef`s so it cannot leak into a consumer. Writing the fully
// qualified inline-namespace path is also unambiguous about WHICH `detail` is
// being reopened — `namespace atx::vol::counters::detail` would be a different
// question, and not one worth relying on a subtle lookup rule to answer.
//
// Both functions are deliberately empty. Nothing needs to happen at run time;
// the assertion is that the symbol RESOLVES. Do not add logic, and do not drop
// `noexcept` — they are called from `noexcept` inline functions.

#include "atx/vol/detail/counters.hpp"
#include "atx/vol/detail/phase_profile.hpp"

// Written as nested blocks, not `namespace a::b::c::detail {}`: a component of a
// nested-namespace-definition is non-inline unless spelled `inline`, and
// reopening the tag that way is an error (-Winline-namespace-reopened-noninline,
// -Werror here). So the `inline` is restated, which is also the honest reading.

namespace atx::vol::counters {
#if defined(ATX_VOL_COUNTERS)
inline namespace counters_on {
#else
inline namespace counters_off {
#endif
namespace detail {

void assert_build_configuration_matches() noexcept {}

} // namespace detail
} // inline namespace counters_on | counters_off
} // namespace atx::vol::counters

namespace atx::vol::phase_profile {
#if defined(ATX_VOL_PROFILE)
inline namespace profile_on {
#else
inline namespace profile_off {
#endif
namespace detail {

void assert_build_configuration_matches() noexcept {}

} // namespace detail
} // inline namespace profile_on | profile_off
} // namespace atx::vol::phase_profile
