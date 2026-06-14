// atx-shm-worker — the dedicated multi-process executor worker (S7-3, §0.8).
//
// ProcessExecutor spawns ONE of these per worker. There is no per-test main() to
// re-exec into (gtest owns the test binary's main, and Windows has no fork), so
// the worker is a SEPARATE thin executable: it REGISTERS the closed set of pure
// workload bodies, then hands control to `run_shm_worker`, which maps the shared
// segments named on argv, drains the cross-process claim cursor, runs each shard
// into its slot, and returns the process exit code (0 clean / 1 failed).
//
// S7-3 shipped exactly one workload — `Test` (a self-contained deterministic
// arithmetic kernel). S7.5b appends the REAL serialized-AlphaStreams workloads
// `Backtests` (run_full_backtest) and `Cpcv` (run_one_fold); the WorkloadId
// dispatch is otherwise unchanged. Eval/Mine land in later units.

#include "atx/engine/parallel/builtin_test_workload.hpp" // test_shard
#include "atx/engine/parallel/executor.hpp"              // register_workload, WorkloadId
#include "atx/engine/parallel/process_executor.hpp"      // run_shm_worker
#include "atx/engine/parallel/workload_streams.hpp" // backtests_shard, cpcv_shard

int main(int argc, char **argv) {
  // Register the closed set of pure shard bodies in THIS process so the worker
  // can resolve the ControlBlock's WorkloadId to a callable (the same registry
  // the parent reasons about — a plain function pointer, process-portable §0.8).
  namespace par = atx::engine::parallel;
  par::register_workload(par::WorkloadId::Test, &par::test_shard);
  par::register_workload(par::WorkloadId::Backtests, &par::backtests_shard);
  par::register_workload(par::WorkloadId::Cpcv, &par::cpcv_shard);
  return par::run_shm_worker(argc, argv);
}
