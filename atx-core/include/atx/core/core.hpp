#pragma once

// =============================================================================
//  atx/core/core.hpp — umbrella header for the atx-core standard library.
//
//  Including this header pulls in every public module, grouped by dependency
//  layer (L0 foundation .. L9 series). Prefer including the specific module
//  header you need in hot translation units — this umbrella is a convenience
//  for application code and a single-TU compile check that all modules coexist.
//
//  Namespaces:
//    atx::core              — foundation + numeric primitives
//    atx::core::container   — fixed/small vector, ring buffer, hash map, list
//    atx::core::concurrent  — spinlock, seqlock, SPSC/MPMC queue, disruptor
//    atx::core::simd        — vectorized span reductions
//    atx::core::stats       — online/rolling/cross-sectional statistics
//    atx::core::linalg      — Eigen aliases + OLS/ridge/WLS regression
//    atx::core::time        — Timestamp/Duration/Date + NYSE calendar
//    atx::core::domain      — Price/Quantity/Notional, Symbol, Bar/Tick
//    atx::core::series      — aligned columnar Column<T> + heterogeneous Frame
// =============================================================================

// ---- L0: foundation -------------------------------------------------------
#include "atx/core/platform.hpp"
#include "atx/core/types.hpp"
#include "atx/core/macro.hpp"
#include "atx/core/bit.hpp"
#include "atx/core/util.hpp"
#include "atx/core/error.hpp"
#include "atx/core/log.hpp"

// ---- L1: numeric primitives -----------------------------------------------
#include "atx/core/safe_math.hpp"
#include "atx/core/math.hpp"
#include "atx/core/hash.hpp"
#include "atx/core/decimal.hpp"
#include "atx/core/random.hpp"

// ---- L2: memory -----------------------------------------------------------
#include "atx/core/aligned.hpp"
#include "atx/core/arena.hpp"
#include "atx/core/object_pool.hpp"

// ---- L3: containers -------------------------------------------------------
#include "atx/core/container/ring_buffer.hpp"
#include "atx/core/container/fixed_vector.hpp"
#include "atx/core/container/small_vector.hpp"
#include "atx/core/container/hash_map.hpp"
#include "atx/core/container/intrusive_list.hpp"

// ---- L4: concurrency ------------------------------------------------------
#include "atx/core/concurrent/spinlock.hpp"
#include "atx/core/concurrent/seqlock.hpp"
#include "atx/core/concurrent/spsc_queue.hpp"
#include "atx/core/concurrent/mpmc_queue.hpp"
#include "atx/core/concurrent/disruptor.hpp"

// ---- L5: SIMD -------------------------------------------------------------
#include "atx/core/simd.hpp"

// ---- L6: statistics & algorithms ------------------------------------------
#include "atx/core/stats/online_stats.hpp"
#include "atx/core/stats/quantile.hpp"
#include "atx/core/stats/algo.hpp"
#include "atx/core/stats/rolling.hpp"
#include "atx/core/stats/cross_section.hpp"

// ---- L7: linear algebra ---------------------------------------------------
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/linalg/decompose.hpp"
#include "atx/core/linalg/solve.hpp"
#include "atx/core/linalg/spd.hpp"
#include "atx/core/linalg/pca.hpp"
#include "atx/core/linalg/regression.hpp"

// ---- L8: time & domain ----------------------------------------------------
#include "atx/core/datetime.hpp"
#include "atx/core/domain/domain.hpp"
#include "atx/core/domain/symbol.hpp"

// ---- L9: series (columnar) ------------------------------------------------
#include "atx/core/series/column.hpp"
#include "atx/core/series/frame.hpp"
