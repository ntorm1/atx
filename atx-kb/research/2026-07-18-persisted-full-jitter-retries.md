# Persisted full-jitter retries for durable event consumers

Date: 2026-07-18

## Reliability gap

Schema v16 durably caps exponential retry backoff, but every consumer with the same policy and attempt number computes the same retry time. A shared outage can therefore synchronize otherwise independent consumers into a retry wave.

AWS describes full jitter as sampling uniformly from zero through the capped exponential backoff window and recommends it to spread retry traffic rather than concentrating it at the deterministic cap. Its architecture comparison reports substantially less client work and server load than unjittered exponential backoff. Google Cloud likewise recommends exponential backoff with jitter and explicitly identifies prevention of synchronized retries as the purpose of the random component.

Primary sources:

- AWS SDK retry behavior: https://docs.aws.amazon.com/sdkref/latest/guide/feature-retry-behavior.html
- AWS Architecture Blog, *Exponential Backoff and Jitter*: https://aws.amazon.com/blogs/architecture/exponential-backoff-and-jitter/
- Google Cloud Storage retry strategy: https://docs.cloud.google.com/storage/docs/retry-strategy

## v17 contract

1. A consumer has an immutable `retry_jitter` policy: `none` or `full`. `none` is the default and preserves v16 behavior.
2. The deterministic window remains `min(base * 2^(attempt - 1), maximum)` with overflow-safe capping.
3. `full` samples an integer delay uniformly from the inclusive range `[0, window]`; `none` uses the window itself.
4. Sampling happens exactly once, inside the transaction that creates a delivery. The chosen `retry_delay_seconds` is persisted both in the delivery audit and in the active consumer head.
5. Exact receive retries return the persisted sample. Lease renewal moves expiry and retry timestamps but never resamples. A retry rejection starts its cooldown at rejection time using the same sample. Backup, restore, terminal dead-lettering, and replay preserve the audit value.
6. Integrity verification recomputes the deterministic window, requires `none` to equal it and `full` to fall within it, and verifies `retry_not_before = expires_at + retry_delay_seconds` exactly.
7. Migration from v16 assigns `none` and backfills deterministic delays for every historical and active delivery before publishing schema version 17.

## Randomness and auditability

Sampling uses SQLite's `randomblob(8)` inside the same database connection. Rejection sampling removes modulo bias over the small bounded delay window. The random value itself is not an external input or a secret; the durable sampled delay is the authoritative replay value.

## Test obligations

- deterministic `none` behavior remains exact;
- full-jitter samples stay in bounds and exhibit spread across independent consumers;
- exact receive retries, renewals, rejections, and backups retain the sampled delay;
- v16 migration backfills active and historical deterministic delays;
- integrity rejects policy, bound, head/audit, or timestamp mismatches;
- CLI registration and JSON output expose policy and sampled delay.
