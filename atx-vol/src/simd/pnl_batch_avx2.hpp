#pragma once

// Internal declaration for the AVX2 Taylor P&L-explain batch kernel. Defined in
// pnl_batch_avx2.cpp (built with -mavx2 -mfma) and called only from
// pnl_batch.cpp behind an atx::vol::simd::have_avx2() guard. Not a public
// header: callers use atx/vol/simd/pnl_batch.hpp.

#include <cstddef>

#include "atx/vol/simd/pnl_batch.hpp" // PnlExplainInputs / PnlExplainOutputs

namespace atx::vol::simd::detail {

void pnl_taylor_explain_batch_avx2(const PnlExplainInputs& in,
                                   const PnlExplainOutputs& out,
                                   std::size_t n) noexcept;

} // namespace atx::vol::simd::detail
