# Incremental execution checkpoint

The original producers and R23 runtime remain unchanged. The successful next
four-batch continuation (661.3s,624,939,008 peak bytes) demonstrates that the
existing256MB file-backed/one-thread native connection fits the0.8GiB external
guard for those actual inputs. It does not establish bounds for all remaining
batches or waive host admission and the40GiB disk floor.

Native compute_new already persists admission only after original source checks,
two complete computations, full typed/schema/population comparison and matching
failure counters. Completed admissions survive interruption. Native build
replays every prior admitted batch on every invocation. Batching more newly
requested units in a single existing build call amortizes those repeated prior
replays without modifying any validation or checkpoint semantics.

The root's new retained-unit audit wraps unchanged admit_unit for one named
existing unit. Each invocation binds current source/runtime/code and the exact
original start/admission/receipt/four payload identities before and after, and
persists an immutable separate result. Successful exact external guard exit is
required independently. Interrupted later audits do not erase earlier completed
results. No unnamed unit receives credit and final complete85 generation
validation is still mandatory. This is the implemented response to the user's
incrementality request; issuer-level streaming remains only a future proposal.

After successful audits of3..6, the next ordinary native continuation may request
eight new units7..14 in one process, using the same original CLI, pins,0.8GiB
guard and40GiB disk floor. It stops on the original resource/source/validation
failures and retains its completed native units. The larger new-unit count
changes scheduling only. It does not allocate more memory or admit a stopped
invocation as successful. Any surviving units need current verification.

Keep the three authorized implementation/review lanes productive during root
data execution. Serialize their offline fixtures under explicit single-worker,
128MB/one-thread connections and original guards. The root remains the only
actual source/build job. Delete only authenticated owned disposable scratch;
retain sources, completed checkpoints and unresolved failure evidence.
