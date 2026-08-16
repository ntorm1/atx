export const meta = {
  name: 'vol-oracle-iter',
  description: 'One transactional SpiderRock capability bootstrap or exact-SHA oracle RSI iteration',
  whenToUse: 'Run exactly one oracle step; canonical mutation occurs only after typed receipts validate.',
  phases: [
    { title: 'Capability', detail: 'freeze canonical SHA and internally verify committed membership digest without membership output' },
    { title: 'Bootstrap', detail: 'one fixed lane, independent review, typed prepare, CAS finalizer, audit' },
    { title: 'Measure', detail: 'ready only: aggregate smoke+tune Mode A+B' },
    { title: 'Attribute', detail: 'ready only: tool-less strict typed aggregate payload' },
    { title: 'Improve', detail: 'ready only: gated vol-sprint exact integration SHA' },
    { title: 'Ratchet', detail: 'ready only: typed exact-SHA holdout prepare, workflow verdict, CAS, audit' },
  ],
}

if (args && args.base && args.base !== 'main') throw new Error('vol-oracle-iter capability probe is fixed to main before canonical creation')
const REQUESTED_BASE = 'main'

function oracleRunId(baseSha, state, nextIter) {
  const step = state === 'ready' ? `ready-${nextIter}` : state
  if (!/^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(String(step || ''))) throw new Error('capability step is not a safe deterministic run identity')
  return `vol-oracle-${baseSha}-${step}`
}
const CANONICAL_REF = 'refs/heads/oracle/canonical'
const ZERO_SHA = '0000000000000000000000000000000000000000'
const RATCHET_GATE_IDS = ['holdout_mode_a', 'holdout_mode_b', 'rel_avx2_speed']
const TARGET_REGISTRY = Object.freeze([
  { metric_id: 'mode_a_price_mae', mode: 'A', unit: 'ticks', limit: 1 },
  { metric_id: 'mode_a_vol_mae', mode: 'A', unit: 'bp', limit: 5 },
  { metric_id: 'mode_a_delta_rel', mode: 'A', unit: 'relative', limit: 0.01 },
  { metric_id: 'mode_a_gamma_rel', mode: 'A', unit: 'relative', limit: 0.01 },
  { metric_id: 'mode_a_theta_rel', mode: 'A', unit: 'relative', limit: 0.01 },
  { metric_id: 'mode_a_vega_rel', mode: 'A', unit: 'relative', limit: 0.01 },
  { metric_id: 'mode_a_rho_rel', mode: 'A', unit: 'relative', limit: 0.01 },
  { metric_id: 'mode_a_phi_rel', mode: 'A', unit: 'relative', limit: 0.01 },
  { metric_id: 'mode_a_volga_rel', mode: 'A', unit: 'relative', limit: 0.01 },
  { metric_id: 'mode_a_vanna_rel', mode: 'A', unit: 'relative', limit: 0.01 },
  { metric_id: 'mode_a_delta_decay_rel', mode: 'A', unit: 'relative', limit: 0.01 },
  { metric_id: 'mode_b_price_mae', mode: 'B', unit: 'ticks', limit: 2 },
  { metric_id: 'mode_b_vol_mae', mode: 'B', unit: 'bp', limit: 10 },
  { metric_id: 'mode_b_delta_rel', mode: 'B', unit: 'relative', limit: 0.02 },
  { metric_id: 'mode_b_gamma_rel', mode: 'B', unit: 'relative', limit: 0.02 },
  { metric_id: 'mode_b_theta_rel', mode: 'B', unit: 'relative', limit: 0.02 },
  { metric_id: 'mode_b_vega_rel', mode: 'B', unit: 'relative', limit: 0.02 },
  { metric_id: 'mode_b_rho_rel', mode: 'B', unit: 'relative', limit: 0.02 },
  { metric_id: 'mode_b_phi_rel', mode: 'B', unit: 'relative', limit: 0.02 },
  { metric_id: 'mode_b_volga_rel', mode: 'B', unit: 'relative', limit: 0.02 },
  { metric_id: 'mode_b_vanna_rel', mode: 'B', unit: 'relative', limit: 0.02 },
  { metric_id: 'mode_b_delta_decay_rel', mode: 'B', unit: 'relative', limit: 0.02 },
])
const AGGREGATE_REGISTRY = Object.freeze([
  { metric_id: 'mode_a_aggregate_error', mode: 'A', unit: 'relative' },
  { metric_id: 'mode_b_aggregate_error', mode: 'B', unit: 'relative' },
])
const SPEED_METRIC_ID = 'rel_avx2_rows_per_second'
const RATCHET_GATE_COMMANDS = Object.freeze({
  holdout_mode_a: 'atx-vol-oracle-bench --cohort holdout --mode A --aggregate-only',
  holdout_mode_b: 'atx-vol-oracle-bench --cohort holdout --mode B --aggregate-only',
  rel_avx2_speed: 'atx-vol-oracle-bench --cohort tune --benchmark-speed --preset rel-avx2 --quiet-host --aggregate-only',
})
const BOOTSTRAP_GATE_COMMANDS = Object.freeze({
  disk: 'powershell scripts\\oracle-bootstrap-preflight.ps1 -Gate disk',
  aggregate_store: 'powershell scripts\\oracle-bootstrap-preflight.ps1 -Gate aggregate_store',
  ingest_manifest: 'powershell scripts\\oracle-bootstrap-preflight.ps1 -Gate ingest_manifest',
  cohort_manifests: 'powershell scripts\\oracle-bootstrap-preflight.ps1 -Gate cohort_manifests',
  holdout_digest: 'powershell scripts\\oracle-bootstrap-preflight.ps1 -Gate holdout_digest',
  mode_a_targeted_tests: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate mode_a_targeted_tests',
  mode_a_smoke: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate mode_a_smoke',
  convention_tests: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate convention_tests',
  mode_a_smoke_tune: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate mode_a_smoke_tune',
  residual_floor: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate residual_floor',
  convention_speed_measure: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate convention_speed_measure',
  convention_speed: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate convention_speed',
  mode_b_targeted_tests: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate mode_b_targeted_tests',
  mode_b_smoke_tune: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate mode_b_smoke_tune',
})
const TARGETED_BOOTSTRAP_GATE_IDS = Object.freeze(['mode_a_targeted_tests', 'mode_a_smoke', 'convention_tests', 'mode_a_smoke_tune', 'residual_floor', 'convention_speed_measure', 'convention_speed', 'mode_b_targeted_tests', 'mode_b_smoke_tune'])
const ORACLE_BENCH_TEST_COUNT = 31
const ORACLE_CONVENTION_TEST_COUNT = 17
const READY_MEASURE_GATES = Object.freeze({
  measure_mode_a: 'atx-vol-oracle-bench --cohort smoke,tune --mode A --scorecard --aggregate-only',
  measure_mode_b: 'atx-vol-oracle-bench --cohort smoke,tune --mode B --scorecard --aggregate-only',
  measure_speed: 'atx-vol-oracle-bench --cohort tune --benchmark-speed --preset rel-avx2 --quiet-host --aggregate-only',
})
const READY_MEASURE_COMMANDS = Object.freeze(Object.values(READY_MEASURE_GATES))
const ADOPTION_COMMAND = 'powershell scripts\\oracle-adopt-existing-data.ps1'
const MODE_A_RECEIPT_ONLY_PATHS = Object.freeze(['atx-vol/bench/oracle/bootstrap/mode-a.json'])
const STAGE1_RECOVERY = Object.freeze({
  source_commit: '58a94584baabae8263d16421f633540b420de10b',
  source_parent: '3025895fea5f569c098090015d90b8b206e8d5a1',
  source_tree: '6a64d8df30456b1dc4ca1e244f29a7affb77c786',
  holdout_digest: '44a7b6641616161ede494a3e0353cb7ae5fb83db65b358b6c803ee915aa9f1c0',
  blobs: Object.freeze({
    'atx-vol/bench/oracle/bootstrap/data.json': 'bb7ce65e891f8f417f4c71af0769ac84b20531fa',
    'atx-vol/bench/oracle/cohorts/holdout.sha256': '66e49a2b4e8835b97e6c2c3d546f345dc751bad0',
  }),
})
const BOOTSTRAP_OPERATION_IDS = Object.freeze({ missing_data: 'bootstrap_data', missing_mode_a: 'bootstrap_mode_a', missing_conventions: 'bootstrap_conventions', missing_mode_b: 'bootstrap_mode_b' })

const EVIDENCE_ITEM = {
  type: 'object', additionalProperties: false, required: ['command', 'exit_code', 'output'],
  properties: { command: { type: 'string' }, exit_code: { type: 'integer' }, output: { type: 'string' } },
}
const BROKER_ROOT_GUARD = {
  type: 'object', additionalProperties: false,
  required: ['main_sha', 'canonical_sha', 'index_sha256', 'tracked_sha256', 'untracked_sha256', 'raw_sha256'],
  properties: {
    main_sha: { type: 'string' }, canonical_sha: { anyOf: [{ type: 'string' }, { type: 'null' }] },
    index_sha256: { type: 'string' }, tracked_sha256: { type: 'string' }, untracked_sha256: { type: 'string' }, raw_sha256: { type: 'string' },
  },
}
const BROKER_EVIDENCE = {
  type: 'object', additionalProperties: false,
  required: ['logical_operation', 'physical_cwd', 'command', 'exit_code', 'output', 'raw_output_sha256', 'root_guard_before', 'root_guard_after'],
  properties: {
    logical_operation: { type: 'string' }, physical_cwd: { type: 'string' }, command: { type: 'string' }, exit_code: { type: 'integer' },
    output: { type: 'string' }, raw_output_sha256: { type: 'string' }, root_guard_before: BROKER_ROOT_GUARD, root_guard_after: BROKER_ROOT_GUARD,
  },
}
const BOOTSTRAP_SUCCESS_EVIDENCE_ITEM = {
  type: 'object', additionalProperties: false, required: ['command', 'exit_code', 'output'],
  properties: {
    command: { type: 'string', pattern: '^[^;&|`()\\r\\n]+$' },
    exit_code: { type: 'integer', enum: [0] }, output: { type: 'string', minLength: 1 },
  },
}
const BOOTSTRAP_DIAGNOSTIC_EVIDENCE_ITEM = {
  type: 'object', additionalProperties: false, required: ['command', 'exit_code', 'output'],
  properties: {
    command: { type: 'string', pattern: '^[^;&|`()\\r\\n]+$' },
    exit_code: { type: 'integer' }, output: { type: 'string', minLength: 1 },
  },
}
const LEASE_RECEIPT = {
  type: 'object', additionalProperties: false,
  required: ['action', 'lease_name', 'run_id', 'branch', 'base_sha', 'worktree', 'heartbeat_id', 'keeper_pid', 'keeper_process_started_utc', 'keeper_ready_utc', 'exit_code', 'output'],
  properties: {
    action: { type: 'string', enum: ['acquire', 'release', 'quarantine'] }, lease_name: { type: 'string' }, run_id: { type: 'string' },
    branch: { type: 'string' }, base_sha: { type: 'string' }, worktree: { type: 'string' }, heartbeat_id: { type: 'string' },
    keeper_pid: { type: 'integer' }, keeper_process_started_utc: { type: 'string' }, keeper_ready_utc: { type: 'string' }, exit_code: { type: 'integer' }, output: { type: 'string' },
  },
}
const BROKER_ACQUIRE = {
  type: 'object', additionalProperties: false,
  required: ['capability', 'operation_id', 'stage', 'lease_name', 'run_id', 'branch', 'base_sha', 'lease_start_sha', 'recovery_replay', 'worktree', 'heartbeat_id', 'keeper_pid', 'keeper_process_started_utc', 'keeper_ready_utc', 'acquisition_receipt', 'broker_evidence'],
  properties: {
    capability: { type: 'string' }, operation_id: { type: 'string' }, stage: { type: 'string' }, lease_name: { type: 'string' }, run_id: { type: 'string' },
    branch: { type: 'string' }, base_sha: { type: 'string' }, lease_start_sha: { type: 'string' }, recovery_replay: { type: 'boolean' }, worktree: { type: 'string' }, heartbeat_id: { type: 'string' }, keeper_pid: { type: 'integer' },
    keeper_process_started_utc: { type: 'string' }, keeper_ready_utc: { type: 'string' }, acquisition_receipt: LEASE_RECEIPT, broker_evidence: BROKER_EVIDENCE,
  },
}
const BROKER_RELEASE = {
  type: 'object', additionalProperties: false,
  required: ['released', 'lease_name', 'sha', 'tree', 'finalize_capability', 'release_receipt', 'broker_evidence'],
  properties: {
    released: { type: 'boolean' }, lease_name: { type: 'string' }, sha: { type: 'string' }, tree: { type: 'string' }, finalize_capability: { type: 'string' },
    release_receipt: LEASE_RECEIPT, broker_evidence: BROKER_EVIDENCE,
  },
}
const QUARANTINE_GATE_RECEIPT = {
  type: 'object', additionalProperties: false,
  required: ['receipt_id', 'gate_id', 'tested_sha', 'tested_tree', 'command', 'exit_code', 'output', 'broker_evidence'],
  properties: {
    receipt_id: { type: 'string' }, gate_id: { type: 'string' }, tested_sha: { type: 'string' }, tested_tree: { type: 'string' },
    command: { type: 'string' }, exit_code: { type: 'integer' }, output: { type: 'string' }, broker_evidence: BROKER_EVIDENCE,
  },
}
const BROKER_QUARANTINE = {
  type: 'object', additionalProperties: false,
  required: ['quarantined', 'lease_name', 'sha', 'tree', 'preserved_paths', 'gate_receipts', 'quarantine_receipt', 'broker_evidence'],
  properties: {
    quarantined: { type: 'boolean' }, lease_name: { type: 'string' }, sha: { type: 'string' }, tree: { type: 'string' },
    preserved_paths: { type: 'array', items: { type: 'string' } }, gate_receipts: { type: 'array', items: QUARANTINE_GATE_RECEIPT },
    quarantine_receipt: LEASE_RECEIPT, broker_evidence: BROKER_EVIDENCE,
  },
}
const HEAD_RECEIPT = {
  type: 'object', additionalProperties: false, required: ['ref', 'sha', 'command', 'exit_code', 'output'],
  properties: { ref: { type: 'string' }, sha: { type: 'string' }, tree: { type: 'string' }, command: { type: 'string' }, exit_code: { type: 'integer' }, output: { type: 'string' }, broker_evidence: BROKER_EVIDENCE },
}
const CAS_RECEIPT = {
  type: 'object', additionalProperties: false, required: ['ref', 'new_sha', 'new_tree', 'expected_old_sha', 'command', 'exit_code', 'output'],
  properties: {
    ref: { type: 'string' }, new_sha: { type: 'string' }, new_tree: { type: 'string' }, expected_old_sha: { type: 'string' }, command: { type: 'string' },
    exit_code: { type: 'integer' }, output: { type: 'string' }, broker_evidence: BROKER_EVIDENCE,
  },
}
const NUMERIC_GATE_METRIC = {
  type: 'object', additionalProperties: false, required: ['metric_id', 'value', 'count', 'unit'],
  properties: {
    metric_id: { type: 'string' }, value: { type: 'number' }, count: { type: 'integer' },
    // Stage 3 floors also carry the population the scale selection ran on. The
    // symmetric selection objective excludes nothing, so it now equals `count`;
    // it stays published so a future narrowing cannot happen silently.
    selection_count: { type: 'integer' }, unit: { type: 'string' }, pin: { type: 'number' },
  },
}
const CONVENTION_MAP = {
  type: 'object', additionalProperties: false,
  required: ['input_model', 'forward_formula', 'rate_model', 'carry_model', 'dividend_model', 'day_count', 'dte_banding_day_count', 'price_scale', 'price_sign', 'vol_scale', 'delta_scale', 'delta_sign', 'gamma_scale', 'gamma_sign', 'theta_basis', 'theta_sign', 'vega_scale', 'vega_sign', 'rho_scale', 'rho_sign', 'phi_scale', 'phi_sign', 'volga_source', 'volga_scale', 'volga_sign', 'vanna_source', 'vanna_scale', 'vanna_sign', 'delta_decay_basis', 'delta_decay_day_count', 'delta_decay_sign'],
  properties: {
    input_model: { type: 'string', enum: ['uprc_spot__rate__sdiv_yield', 'discrete_forward_pv__rate__sdiv_yield', 'discrete_forward_net_carry__rate__sdiv_yield', 'discrete_forward__rate__sdiv_yield', 'discrete_forward__rate_minus_sdiv__zero_carry', 'discrete_forward__zero_rate__zero_carry', 'discrete_forward_pv__rate_minus_sdiv__zero_carry', 'discrete_forward_pv__rate_plus_sdiv__zero_carry'] },
    forward_formula: { type: 'string', enum: ['none', 'uprc_exp_rate_t_minus_ddiv'] },
    rate_model: { type: 'string', enum: ['continuous_row_rate', 'continuous_rate_minus_sdiv', 'continuous_rate_plus_sdiv', 'zero'] },
    carry_model: { type: 'string', enum: ['sdiv_as_yield', 'zero'] },
    dividend_model: { type: 'string', enum: ['continuous_yield_only', 'discrete_cash_forward'] },
    // `day_count` is theta's, derived from the multiplier production applies.
    // `dte_banding_day_count` is the scorecard's calendar banding day count,
    // outside the search but recorded so a silent change to it is visible.
    day_count: { type: 'string', enum: ['ACT_365F', 'ACT_365_25', 'ACT_360', 'BUS_252'] },
    dte_banding_day_count: { type: 'string', enum: ['ACT_365F', 'ACT_365_25', 'ACT_360', 'BUS_252'] },
    price_scale: { type: 'string', enum: ['per_share', 'per_contract_100', 'per_share_from_contract'] },
    price_sign: { type: 'string', enum: ['positive', 'negative'] }, vol_scale: { type: 'string', enum: ['decimal_identity'] },
    delta_scale: { type: 'string' }, delta_sign: { type: 'string', enum: ['positive', 'negative'] },
    gamma_scale: { type: 'string' }, gamma_sign: { type: 'string', enum: ['positive', 'negative'] },
    theta_basis: { type: 'string', enum: ['per_day', 'per_year'] }, theta_sign: { type: 'string', enum: ['positive', 'negative'] },
    vega_scale: { type: 'string' }, vega_sign: { type: 'string', enum: ['positive', 'negative'] },
    rho_scale: { type: 'string' }, rho_sign: { type: 'string', enum: ['positive', 'negative'] },
    phi_scale: { type: 'string' }, phi_sign: { type: 'string', enum: ['positive', 'negative'] },
    volga_source: { type: 'string', enum: ['volga', 'vanna'] }, volga_scale: { type: 'string' }, volga_sign: { type: 'string', enum: ['positive', 'negative'] },
    vanna_source: { type: 'string', enum: ['volga', 'vanna'] }, vanna_scale: { type: 'string' }, vanna_sign: { type: 'string', enum: ['positive', 'negative'] },
    delta_decay_basis: { type: 'string', enum: ['per_day', 'per_year'] }, delta_decay_day_count: { type: 'string', enum: ['ACT_365F', 'ACT_365_25', 'ACT_360', 'BUS_252'] }, delta_decay_sign: { type: 'string', enum: ['positive', 'negative'] },
  },
}
const STAGE3_DELTA = {
  type: 'object', additionalProperties: false, required: ['metric_id', 'candidate', 'baseline', 'delta', 'count', 'unit'],
  properties: { metric_id: { type: 'string' }, candidate: { type: 'number' }, baseline: { type: 'number' }, delta: { type: 'number' }, count: { type: 'integer' }, unit: { type: 'string' } },
}
const STAGE3_CANDIDATE_PRICE = {
  type: 'object', additionalProperties: false, required: ['candidate_id', 'smoke_price_mae_ticks', 'smoke_count', 'tune_sample_price_mae_ticks', 'tune_sample_count'],
  properties: { candidate_id: { type: 'string' }, smoke_price_mae_ticks: { type: 'number' }, smoke_count: { type: 'integer' }, tune_sample_price_mae_ticks: { type: 'number' }, tune_sample_count: { type: 'integer' } },
}
// `pin` is present on convention_speed and ABSENT on convention_speed_measure,
// which produces the baseline iter-000's pin is DERIVED from as
// floor(baseline * 0.90); validGateReceipt enforces presence/absence per gate id.
const STAGE3_SPEED = {
  type: 'object', additionalProperties: false, required: ['metric_id', 'value', 'count', 'unit', 'preset', 'quiet_host'],
  properties: { metric_id: { type: 'string' }, value: { type: 'number' }, count: { type: 'integer' }, unit: { type: 'string' }, pin: { type: 'number' }, preset: { type: 'string' }, quiet_host: { type: 'boolean' } },
}
const STAGE3_DIAGNOSTIC_SPEED = {
  type: 'object', additionalProperties: false, required: ['preset', 'citable', 'wall_seconds', 'rows_per_second'],
  properties: { preset: { type: 'string', enum: ['dev'] }, citable: { type: 'boolean', enum: [false] }, wall_seconds: { type: 'number' }, rows_per_second: { type: 'number' } },
}
const GATE_RECEIPT = {
  type: 'object', additionalProperties: false, required: ['receipt_id', 'gate_id', 'tested_sha', 'tested_tree', 'command', 'exit_code', 'output', 'result', 'broker_evidence'],
  properties: {
    receipt_id: { type: 'string' }, gate_id: { type: 'string', enum: Object.keys(BOOTSTRAP_GATE_COMMANDS) }, tested_sha: { type: 'string' }, tested_tree: { type: 'string' }, command: { type: 'string' }, exit_code: { type: 'integer' }, output: { type: 'string' }, broker_evidence: BROKER_EVIDENCE,
    result: {
      type: 'object', additionalProperties: false, required: ['schema_version', 'status', 'observations', 'command_id', 'raw_output_sha256'],
      properties: {
        schema_version: { type: 'integer', enum: [1] }, status: { type: 'string', enum: ['PASS'] }, observations: { type: 'integer' },
        command_id: { type: 'string', enum: Object.keys(BOOTSTRAP_GATE_COMMANDS) }, raw_output_sha256: { type: 'string', pattern: '^[0-9a-f]{64}$' },
        tested_sha: { type: 'string' }, tested_tree: { type: 'string' },
        gate_kind: { type: 'string', enum: ['ctest', 'oracle_bench', 'oracle_convention', 'oracle_floor_verify', 'oracle_speed'] }, tests_executed: { type: 'integer' }, tests_passed: { type: 'integer' },
        rows_processed: { type: 'integer' }, metric_ids: { type: 'array', items: { type: 'string' } }, audit_summary: { type: 'string' },
        // Stage 3 only: the sweep's own row accounting, so
        // rows_total == rows_processed + engine_errors is re-checkable from the
        // receipt without re-reading the artifact.
        rows_total: { type: 'integer' }, engine_errors: { type: 'integer' },
        // TWO floor arrays, deliberately not one. `metrics` is the
        // STANDARD-RELATIVE floor, |m-o|/max(|o|,floor), kept because the
        // charter states its Greek target relative to the oracle.
        // `symmetric_metrics` is |m-o|/max(|m|,|o|,floor) — the loss the scale
        // selection minimises, bounded and free of the smallest-scale gradient —
        // and it is the array the no-regression gate and the ratchet baseline
        // are stated against, so the gate cannot contradict the selector.
        metrics: { type: 'array', items: NUMERIC_GATE_METRIC }, baseline_metrics: { type: 'array', items: NUMERIC_GATE_METRIC },
        metric_deltas: { type: 'array', items: STAGE3_DELTA },
        symmetric_metrics: { type: 'array', items: NUMERIC_GATE_METRIC }, baseline_symmetric_metrics: { type: 'array', items: NUMERIC_GATE_METRIC },
        symmetric_metric_deltas: { type: 'array', items: STAGE3_DELTA },
        conventions: CONVENTION_MAP,
        // What winning_convention() actually prices with. The gate fails closed
        // while it differs from `conventions`.
        production_conventions: CONVENTION_MAP,
        candidate_prices: { type: 'array', items: STAGE3_CANDIDATE_PRICE },
        // Greeks the SELECTED input model still regresses on versus baseline.
        // Non-empty only when both finalists regressed and the lexicographic
        // rank fell through to price MAE; the trade-off is published, not silent.
        input_model_regressed_greeks: { type: 'array', items: { type: 'string' } },
        diagnostic_speed: STAGE3_DIAGNOSTIC_SPEED, speed: STAGE3_SPEED,
      },
    },
  },
}
const ADOPTION_RECEIPT = {
  type: 'object', additionalProperties: false, required: ['command', 'exit_code', 'output', 'result'],
  properties: {
    command: { type: 'string', enum: [ADOPTION_COMMAND] }, exit_code: { type: 'integer' }, output: { type: 'string' },
    result: {
      type: 'object', additionalProperties: false, required: ['schema_version', 'status', 'command_id'],
      properties: {
        schema_version: { type: 'integer', enum: [1] }, status: { type: 'string', enum: ['ADOPTED', 'INGEST_REQUIRED'] },
        command_id: { type: 'string', enum: ['oracle_existing_store_adoption'] }, reason: { type: 'string' }, base_sha: { type: 'string' },
        manifest_sha256: { type: 'string' }, holdout_membership_sha256: { type: 'string' }, total_rows: { type: 'integer' },
        bucket_count: { type: 'integer' }, parquet_files: { type: 'integer' }, cohort_underlier_count: { type: 'integer' },
      },
    },
  },
}
const PRECHECK_GATE_RECEIPT = {
  type: 'object', additionalProperties: false,
  required: ['receipt_id', 'gate_id', 'tested_sha', 'tested_tree', 'command', 'status', 'exit_code', 'output', 'broker_evidence'],
  properties: {
    receipt_id: { type: 'string' }, gate_id: { type: 'string', enum: ['mode_a_targeted_tests', 'mode_a_smoke'] },
    tested_sha: { type: 'string' }, tested_tree: { type: 'string' }, command: { type: 'string' },
    status: { type: 'string', enum: ['PASS', 'FAIL'] }, exit_code: { type: 'integer' }, output: { type: 'string' },
    result: GATE_RECEIPT.properties.result, broker_evidence: BROKER_EVIDENCE,
  },
}
const CHANGED_PATH_RECEIPT = {
  type: 'object', additionalProperties: false, required: ['base_sha', 'tested_sha', 'command', 'exit_code', 'output', 'paths'],
  properties: {
    base_sha: { type: 'string' }, tested_sha: { type: 'string' }, command: { type: 'string' }, exit_code: { type: 'integer' },
    output: { type: 'string' }, paths: { type: 'array', items: { type: 'string' } },
  },
}
const RECOVERY_GATE_RECEIPT = {
  type: 'object', additionalProperties: false,
  required: ['receipt_id', 'gate_id', 'tested_sha', 'tested_tree', 'command', 'exit_code', 'output', 'broker_evidence'],
  properties: {
    receipt_id: { type: 'string' }, gate_id: { type: 'string' }, tested_sha: { type: 'string' }, tested_tree: { type: 'string' },
    command: { type: 'string' }, exit_code: { type: 'integer' }, output: { type: 'string' }, broker_evidence: BROKER_EVIDENCE,
  },
}
const RECOVERY_SOURCE_RECEIPT = {
  type: 'object', additionalProperties: false,
  required: ['source_commit', 'source_parent', 'source_tree', 'adoption_rerun', 'blobs'],
  properties: {
    source_commit: { type: 'string' }, source_parent: { type: 'string' }, source_tree: { type: 'string' }, adoption_rerun: { type: 'boolean' },
    blobs: { type: 'array', items: { type: 'object', additionalProperties: false, required: ['path', 'source_blob', 'replay_blob'], properties: { path: { type: 'string' }, source_blob: { type: 'string' }, replay_blob: { type: 'string' } } } },
  },
}
const SEALED_RECOVERY_RESULT = {
  type: 'object', additionalProperties: false,
  required: ['recovery_id', 'replayed', 'recovery', 'replay_paths', 'gate_receipts', 'sha', 'tree', 'files_changed', 'changed_path_receipt', 'broker_evidence'],
  properties: {
    recovery_id: { type: 'string' }, replayed: { type: 'boolean' }, recovery: RECOVERY_SOURCE_RECEIPT,
    replay_paths: { type: 'array', items: { type: 'string' } }, gate_receipts: { type: 'array', items: RECOVERY_GATE_RECEIPT },
    sha: { type: 'string' }, tree: { type: 'string' }, files_changed: { type: 'array', items: { type: 'string' } },
    changed_path_receipt: CHANGED_PATH_RECEIPT, broker_evidence: { type: 'array', items: BROKER_EVIDENCE },
  },
}
const RECOVERY_QUERY = {
  type: 'object', additionalProperties: false, required: ['found', 'recovery_id', 'broker_evidence'],
  properties: {
    found: { type: 'boolean' }, recovery_id: { type: 'string' }, result: SEALED_RECOVERY_RESULT, broker_evidence: BROKER_EVIDENCE,
  },
}
const MEASURE_GATE_RECEIPT = {
  type: 'object', additionalProperties: false, required: ['gate_id', 'command', 'exit_code', 'output', 'result'],
  properties: {
    gate_id: { type: 'string', enum: Object.keys(READY_MEASURE_GATES) }, command: { type: 'string' }, exit_code: { type: 'integer' }, output: { type: 'string' },
    result: {
      type: 'object', additionalProperties: false, required: ['schema_version', 'status', 'command_id', 'observations', 'metrics'],
      properties: {
        schema_version: { type: 'integer', enum: [1] }, status: { type: 'string', enum: ['PASS'] }, observations: { type: 'integer' },
        command_id: { type: 'string', enum: Object.keys(READY_MEASURE_GATES) },
        metrics: { type: 'array', items: NUMERIC_GATE_METRIC },
      },
    },
  },
}
const RATCHET_GATE_RECEIPT = {
  type: 'object', additionalProperties: false, required: ['gate_id', 'command', 'exit_code', 'output', 'result'],
  properties: {
    gate_id: { type: 'string', enum: RATCHET_GATE_IDS }, command: { type: 'string' }, exit_code: { type: 'integer' }, output: { type: 'string' },
    result: {
      type: 'object', additionalProperties: false, required: ['schema_version', 'status', 'command_id', 'observations', 'metrics'],
      properties: {
        schema_version: { type: 'integer', enum: [1] }, status: { type: 'string', enum: ['PASS'] }, observations: { type: 'integer' },
        command_id: { type: 'string', enum: RATCHET_GATE_IDS },
        metrics: { type: 'array', items: NUMERIC_GATE_METRIC },
      },
    },
  },
}
const INTEGRATION_RECEIPT = {
  type: 'object', additionalProperties: false, required: ['reviewed_sha', 'reviewed_tree', 'head_after', 'tree_after', 'command', 'exit_code', 'output'],
  properties: {
    reviewed_sha: { type: 'string' }, reviewed_tree: { type: 'string' }, head_after: { type: 'string' }, tree_after: { type: 'string' }, command: { type: 'string' },
    exit_code: { type: 'integer' }, output: { type: 'string' },
  },
}
const FINDING = {
  type: 'object', additionalProperties: false, required: ['location', 'severity', 'problem', 'fix'],
  properties: {
    location: { type: 'string' }, severity: { type: 'string', enum: ['blocker', 'major', 'minor'] },
    problem: { type: 'string' }, fix: { type: 'string' },
  },
}
const CAPABILITY = {
  type: 'object', additionalProperties: false,
  required: ['state', 'canonical_ref', 'canonical_exists', 'base_ref', 'base_sha', 'base_tree', 'holdout_digest_receipt', 'next_iter', 'evidence', 'broker_evidence'],
  properties: {
    state: { type: 'string', enum: ['missing_data', 'missing_mode_a', 'missing_conventions', 'missing_mode_b', 'ready'] },
    canonical_ref: { type: 'string' }, canonical_exists: { type: 'boolean' }, base_ref: { type: 'string' }, base_sha: { type: 'string' }, base_tree: { type: 'string' },
    holdout_digest_receipt: { type: 'string' }, next_iter: { type: 'string' }, evidence: { type: 'array', items: EVIDENCE_ITEM },
    diagnostics: { type: 'array', items: EVIDENCE_ITEM }, broker_evidence: BROKER_EVIDENCE,
  },
}
const BOOTSTRAP_REPORT_IDENTITY_REQUIRED = ['state', 'outcome', 'branch', 'sha', 'tree', 'base_sha', 'worktree', 'lease_name', 'lease_run_id', 'heartbeat_id', 'keeper_pid', 'keeper_process_started_utc', 'acquisition_receipt', 'holdout_digest_receipt', 'evidence', 'broker_evidence', 'deviations']
const BOOTSTRAP_REPORT_COMMON_PROPERTIES = {
  state: { type: 'string', enum: ['missing_data', 'missing_mode_a', 'missing_conventions', 'missing_mode_b'] },
  branch: { type: 'string', minLength: 1 }, sha: { type: 'string' }, tree: { type: 'string' }, base_sha: { type: 'string', pattern: '^[0-9a-f]{40}$' },
  worktree: { type: 'string', minLength: 1 }, lease_name: { type: 'string', pattern: '^pool-[0-9]+$' },
  lease_run_id: { type: 'string', minLength: 1 }, heartbeat_id: { type: 'string', minLength: 1 },
  keeper_pid: { type: 'integer' }, keeper_process_started_utc: { type: 'string', minLength: 1 }, acquisition_receipt: LEASE_RECEIPT,
  bootstrap_path: { type: 'string', enum: ['data_recovery', 'data_ingest', 'mode_a_receipt_only', 'mode_a_implementation', 'standard'] },
  recovery_id: { type: 'string' }, recovery_replayed: { type: 'boolean' },
  adoption_receipt: ADOPTION_RECEIPT, disk_receipt: GATE_RECEIPT,
  recovery_source_receipt: {
    type: 'object', additionalProperties: false,
    required: ['source_commit', 'source_parent', 'source_tree', 'adoption_rerun', 'blobs'],
    properties: {
      source_commit: { type: 'string' }, source_parent: { type: 'string' }, source_tree: { type: 'string' }, adoption_rerun: { type: 'boolean', enum: [false] },
      blobs: { type: 'array', items: { type: 'object', additionalProperties: false, required: ['path', 'source_blob', 'replay_blob'], properties: { path: { type: 'string' }, source_blob: { type: 'string' }, replay_blob: { type: 'string' } } } },
    },
  },
  broker_evidence: { type: 'array', minItems: 1, items: BROKER_EVIDENCE },
  precheck_gate_receipts: { type: 'array', items: PRECHECK_GATE_RECEIPT }, changed_path_receipt: CHANGED_PATH_RECEIPT,
  deviations: { type: 'string' },
}
const BOOTSTRAP_REPORT_VARIANTS = {
  anyOf: [
    {
      type: 'object', additionalProperties: false,
      required: BOOTSTRAP_REPORT_IDENTITY_REQUIRED,
      properties: {
        ...BOOTSTRAP_REPORT_COMMON_PROPERTIES,
        outcome: { type: 'string', enum: ['DONE'] }, sha: { type: 'string', pattern: '^[0-9a-f]{40}$' }, tree: { type: 'string', pattern: '^[0-9a-f]{40}$' },
        holdout_digest_receipt: { type: 'string', pattern: '^[0-9a-f]{64}$' },
        evidence: { type: 'array', minItems: 1, items: BOOTSTRAP_SUCCESS_EVIDENCE_ITEM },
        diagnostics: { type: 'array', items: BOOTSTRAP_DIAGNOSTIC_EVIDENCE_ITEM },
      },
    },
    {
      type: 'object', additionalProperties: false,
      required: [...BOOTSTRAP_REPORT_IDENTITY_REQUIRED, 'blockers', 'diagnostics'],
      properties: {
        ...BOOTSTRAP_REPORT_COMMON_PROPERTIES,
        outcome: { type: 'string', enum: ['BLOCKED'] }, sha: { type: 'string', pattern: '^(?:|[0-9a-f]{40})$' }, tree: { type: 'string', pattern: '^(?:|[0-9a-f]{40})$' },
        holdout_digest_receipt: { type: 'string', pattern: '^(?:|[0-9a-f]{64})$' },
        evidence: { type: 'array', items: BOOTSTRAP_SUCCESS_EVIDENCE_ITEM },
        blockers: { type: 'array', minItems: 1, items: { type: 'string', minLength: 1 } },
        diagnostics: { type: 'array', minItems: 1, items: BOOTSTRAP_DIAGNOSTIC_EVIDENCE_ITEM },
      },
    },
  ],
}
const BOOTSTRAP_WIRE_SCHEMA_KEYS = new Set([
  '$schema', 'type', 'description', 'title', 'properties', 'required',
  'additionalProperties', 'items', 'enum', 'const', 'anyOf',
])
function bootstrapWireSchema(schema) {
  if (Array.isArray(schema)) return schema.map(item => bootstrapWireSchema(item))
  if (!schema || typeof schema !== 'object') return schema
  const converted = {}
  for (const [key, value] of Object.entries(schema)) {
    if (!BOOTSTRAP_WIRE_SCHEMA_KEYS.has(key)) continue
    if (key === 'properties') {
      converted.properties = Object.fromEntries(Object.entries(value).map(([name, child]) => [name, bootstrapWireSchema(child)]))
    } else if (key === 'items') converted.items = bootstrapWireSchema(value)
    else if (key === 'anyOf') converted.anyOf = value.map(item => bootstrapWireSchema(item))
    else converted[key] = Array.isArray(value) ? [...value] : value
  }
  return converted
}
const BOOTSTRAP_REPORT_TOOL_SCHEMA = {
  type: 'object', additionalProperties: false, required: ['report'],
  properties: { report: bootstrapWireSchema(BOOTSTRAP_REPORT_VARIANTS) },
}
const REVIEW = {
  type: 'object', additionalProperties: false, required: ['verdict', 'reviewed_sha', 'evidence', 'broker_evidence', 'findings'],
  properties: {
    verdict: { type: 'string', enum: ['APPROVE', 'BLOCK'] }, reviewed_sha: { type: 'string' },
    evidence: { type: 'array', items: EVIDENCE_ITEM }, diagnostics: { type: 'array', items: EVIDENCE_ITEM }, broker_evidence: { type: 'array', minItems: 1, items: BROKER_EVIDENCE },
    findings: { type: 'array', items: FINDING },
  },
}
const CLEANUP = {
  type: 'object', additionalProperties: false, required: ['passed', 'released', 'release_receipts', 'evidence'],
  properties: {
    passed: { type: 'boolean' }, released: { type: 'array', items: { type: 'string' } }, release_receipts: { type: 'array', items: LEASE_RECEIPT },
    evidence: { type: 'array', items: EVIDENCE_ITEM }, diagnostics: { type: 'array', items: EVIDENCE_ITEM },
  },
}
const BOOTSTRAP_PREPARE = {
  type: 'object', additionalProperties: false,
  required: ['passed', 'reviewed_sha', 'integration_branch', 'integration_sha', 'integration_worktree', 'integration_lease', 'lease_run_id', 'integration_heartbeat_id', 'keeper_pid', 'keeper_process_started_utc', 'holdout_digest_receipt', 'next_state', 'lane_release_receipt', 'acquisition_receipt', 'integration_receipt', 'head_receipt', 'gate_receipts', 'integration_release_receipt', 'evidence'],
  properties: {
    passed: { type: 'boolean' }, reviewed_sha: { type: 'string' }, integration_branch: { type: 'string' }, integration_sha: { type: 'string' },
    integration_worktree: { type: 'string' }, integration_lease: { type: 'string' }, lease_run_id: { type: 'string' },
    integration_heartbeat_id: { type: 'string' }, keeper_pid: { type: 'integer' }, keeper_process_started_utc: { type: 'string' },
    holdout_digest_receipt: { type: 'string' }, next_state: { type: 'string', enum: ['missing_mode_a', 'missing_conventions', 'missing_mode_b', 'ready'] },
    lane_release_receipt: LEASE_RECEIPT, acquisition_receipt: LEASE_RECEIPT, integration_receipt: INTEGRATION_RECEIPT,
    head_receipt: HEAD_RECEIPT, gate_receipts: { type: 'array', items: GATE_RECEIPT }, integration_release_receipt: LEASE_RECEIPT,
    evidence: { type: 'array', items: EVIDENCE_ITEM }, diagnostics: { type: 'array', items: EVIDENCE_ITEM },
  },
}
const BOOTSTRAP_VERIFY = {
  type: 'object', additionalProperties: false,
  required: ['passed', 'reviewed_sha', 'reviewed_tree', 'integration_branch', 'integration_sha', 'integration_tree', 'integration_worktree', 'integration_lease', 'lease_run_id', 'integration_heartbeat_id', 'keeper_pid', 'keeper_process_started_utc', 'holdout_digest_receipt', 'next_state', 'integration_receipt', 'head_receipt', 'gate_receipts', 'broker_evidence'],
  properties: {
    passed: { type: 'boolean' }, reviewed_sha: { type: 'string' }, reviewed_tree: { type: 'string' }, integration_branch: { type: 'string' }, integration_sha: { type: 'string' }, integration_tree: { type: 'string' },
    integration_worktree: { type: 'string' }, integration_lease: { type: 'string' }, lease_run_id: { type: 'string' }, integration_heartbeat_id: { type: 'string' },
    keeper_pid: { type: 'integer' }, keeper_process_started_utc: { type: 'string' }, holdout_digest_receipt: { type: 'string' },
    next_state: { type: 'string', enum: ['missing_mode_a', 'missing_conventions', 'missing_mode_b', 'ready'] }, integration_receipt: INTEGRATION_RECEIPT,
    head_receipt: HEAD_RECEIPT, gate_receipts: { type: 'array', items: GATE_RECEIPT },
    diagnostics: { type: 'array', items: EVIDENCE_ITEM }, broker_evidence: { type: 'array', minItems: 1, items: BROKER_EVIDENCE },
  },
}
const BASELINE_METRIC = {
  type: 'object', additionalProperties: false,
  required: ['metric_id', 'mode', 'baseline', 'count', 'unit'],
  properties: {
    metric_id: { type: 'string', enum: TARGET_REGISTRY.map(item => item.metric_id) }, mode: { type: 'string', enum: ['A', 'B'] },
    baseline: { type: 'number' }, count: { type: 'integer' }, unit: { type: 'string', enum: ['ticks', 'bp', 'relative'] },
  },
}
const AGGREGATE_BASELINE = {
  type: 'object', additionalProperties: false, required: ['metric_id', 'mode', 'baseline', 'count', 'unit'],
  properties: {
    metric_id: { type: 'string', enum: AGGREGATE_REGISTRY.map(item => item.metric_id) }, mode: { type: 'string', enum: ['A', 'B'] },
    baseline: { type: 'number' }, count: { type: 'integer' }, unit: { type: 'string', enum: ['relative'] },
  },
}
const SPEED_BASELINE = {
  type: 'object', additionalProperties: false, required: ['metric_id', 'baseline', 'pin', 'unit'],
  properties: {
    metric_id: { type: 'string', enum: [SPEED_METRIC_ID] }, baseline: { type: 'number' }, pin: { type: 'number' },
    unit: { type: 'string', enum: ['rows_per_second'] },
  },
}
const ATTR_PAYLOAD = {
  type: 'object', additionalProperties: false,
  required: ['schema_version', 'iteration', 'target_metrics', 'aggregate_metrics', 'speed', 'prior_refuted_ids', 'oracle_suspect_cells', 'conventions'],
  properties: {
    schema_version: { type: 'integer', enum: [2] }, iteration: { type: 'integer' },
    target_metrics: { type: 'array', items: BASELINE_METRIC }, aggregate_metrics: { type: 'array', items: AGGREGATE_BASELINE }, speed: SPEED_BASELINE,
    prior_refuted_ids: { type: 'array', items: { type: 'integer', minimum: 0, maximum: 2147483647 } },
    oracle_suspect_cells: { type: 'array', items: { type: 'integer', minimum: 0, maximum: 2147483647 } }, conventions: CONVENTION_MAP,
  },
}
const ANALYSIS_CONTEXT = {
  type: 'object', additionalProperties: false,
  required: ['prior_refuted_ids', 'oracle_suspect_cells', 'conventions'],
  properties: {
    prior_refuted_ids: ATTR_PAYLOAD.properties.prior_refuted_ids,
    oracle_suspect_cells: ATTR_PAYLOAD.properties.oracle_suspect_cells,
    conventions: CONVENTION_MAP,
  },
}
const MEASURE = {
  type: 'object', additionalProperties: false,
  required: ['status', 'iter', 'scorecard_path', 'branch', 'sha', 'base_sha', 'worktree', 'lease_name', 'lease_run_id', 'heartbeat_id', 'keeper_pid', 'keeper_process_started_utc', 'acquisition_receipt', 'release_receipt', 'lease_released', 'analysis_context', 'gate_receipts', 'evidence'],
  properties: {
    status: { type: 'string', enum: ['ok', 'failed'] }, iter: { type: 'string' }, scorecard_path: { type: 'string' }, branch: { type: 'string' },
    sha: { type: 'string' }, base_sha: { type: 'string' }, worktree: { type: 'string' }, lease_name: { type: 'string' }, lease_run_id: { type: 'string' },
    heartbeat_id: { type: 'string' }, keeper_pid: { type: 'integer' }, keeper_process_started_utc: { type: 'string' },
    acquisition_receipt: LEASE_RECEIPT, release_receipt: LEASE_RECEIPT, lease_released: { type: 'boolean' }, analysis_context: ANALYSIS_CONTEXT,
    gate_receipts: { type: 'array', items: MEASURE_GATE_RECEIPT },
    evidence: { type: 'array', items: EVIDENCE_ITEM }, diagnostics: { type: 'array', items: EVIDENCE_ITEM },
  },
}
const ATTR = {
  type: 'object', additionalProperties: false, required: ['hypotheses', 'new_suspect_candidates'],
  properties: {
    hypotheses: { type: 'array', items: {
      type: 'object', additionalProperties: false, required: ['id', 'target_metric_ids', 'mechanism', 'prediction', 'blast_radius', 'effort'],
      properties: {
        id: { type: 'string', pattern: '^H-[A-Z0-9-]{1,48}$' }, target_metric_ids: { type: 'array', items: { type: 'string', enum: TARGET_REGISTRY.map(item => item.metric_id) } },
        mechanism: { type: 'string' }, prediction: { type: 'string' }, blast_radius: { type: 'string' }, effort: { type: 'string', enum: ['S', 'M', 'L'] },
      },
    } },
    new_suspect_candidates: { type: 'array', items: { type: 'string' } },
  },
}
const RATCHET_METRIC = {
  type: 'object', additionalProperties: false,
  required: ['metric_id', 'mode', 'gate', 'direction', 'baseline', 'candidate', 'delta', 'pin', 'unit', 'evidence_index'],
  properties: {
    metric_id: { type: 'string' }, mode: { type: 'string', enum: ['A', 'B', 'ALL'] }, gate: { type: 'string', enum: ['target', 'aggregate', 'speed'] },
    direction: { type: 'string', enum: ['lower', 'higher'] }, baseline: { type: 'number' }, candidate: { type: 'number' }, delta: { type: 'number' },
    pin: { type: 'number' }, unit: { type: 'string' }, evidence_index: { type: 'integer' },
  },
}
const DIGEST_RECEIPT = {
  type: 'object', additionalProperties: false, required: ['expected_digest', 'actual_digest', 'command', 'exit_code', 'output'],
  properties: {
    expected_digest: { type: 'string' }, actual_digest: { type: 'string' }, command: { type: 'string' },
    exit_code: { type: 'integer' }, output: { type: 'string' },
  },
}
const MARKET_RECEIPT = {
  type: 'object', additionalProperties: false,
  required: ['cell_id', 'nbbo_bid_mean', 'nbbo_ask_mean', 'atx_price_mean', 'oracle_price_mean', 'atx_nbbo_distance', 'oracle_nbbo_distance', 'sample_count', 'command', 'exit_code', 'output'],
  properties: {
    cell_id: { type: 'integer', minimum: 0, maximum: 2147483647 }, nbbo_bid_mean: { type: 'number' }, nbbo_ask_mean: { type: 'number' },
    atx_price_mean: { type: 'number' }, oracle_price_mean: { type: 'number' }, atx_nbbo_distance: { type: 'number' },
    oracle_nbbo_distance: { type: 'number' }, sample_count: { type: 'integer' }, command: { type: 'string' }, exit_code: { type: 'integer' }, output: { type: 'string' },
  },
}
const RATCHET_PREPARE = {
  type: 'object', additionalProperties: false,
  required: ['tested_branch', 'tested_sha', 'base_sha', 'ratchet_branch', 'ratchet_sha', 'worktree', 'lease_name', 'lease_run_id', 'heartbeat_id', 'keeper_pid', 'keeper_process_started_utc', 'acquisition_receipt', 'release_receipt', 'digest_receipt', 'applicable_modes', 'metrics', 'metric_evidence', 'gate_receipts', 'oracle_suspects_excluded', 'market_evidence', 'memory_verdict', 'holdout_summary', 'hypotheses_confirmed', 'hypotheses_refuted', 'ledger_appended', 'northstar_updated', 'evidence'],
  properties: {
    tested_branch: { type: 'string' }, tested_sha: { type: 'string' }, base_sha: { type: 'string' }, ratchet_branch: { type: 'string' },
    ratchet_sha: { type: 'string' }, worktree: { type: 'string' }, lease_name: { type: 'string' }, lease_run_id: { type: 'string' },
    heartbeat_id: { type: 'string' }, keeper_pid: { type: 'integer' }, keeper_process_started_utc: { type: 'string' },
    acquisition_receipt: LEASE_RECEIPT, release_receipt: LEASE_RECEIPT, digest_receipt: DIGEST_RECEIPT,
    applicable_modes: { type: 'array', items: { type: 'string', enum: ['A', 'B'] } }, metrics: { type: 'array', items: RATCHET_METRIC },
    metric_evidence: { type: 'array', items: EVIDENCE_ITEM }, gate_receipts: { type: 'array', items: RATCHET_GATE_RECEIPT },
    oracle_suspects_excluded: { type: 'array', items: { type: 'integer', minimum: 0, maximum: 2147483647 } }, market_evidence: { type: 'array', items: MARKET_RECEIPT },
    memory_verdict: { type: 'string', enum: ['ACCEPT', 'REJECT'] }, holdout_summary: { type: 'string' },
    hypotheses_confirmed: { type: 'array', items: { type: 'string' } }, hypotheses_refuted: { type: 'array', items: { type: 'string' } },
    ledger_appended: { type: 'array', items: { type: 'string' } }, northstar_updated: { type: 'boolean' },
    evidence: { type: 'array', items: EVIDENCE_ITEM }, diagnostics: { type: 'array', items: EVIDENCE_ITEM },
  },
}

const BOOTSTRAP_LANES = {
  missing_data: { stage: '1', slug: 'data', next: 'missing_mode_a', gate_ids: ['aggregate_store', 'ingest_manifest', 'cohort_manifests', 'holdout_digest'], contract: 'Use only broker recover_stage1 for immutable source 58a94584baabae8263d16421f633540b420de10b. It validates the exact parent/tree/two blobs, replays those blobs atop the frozen fixed main, runs the four fixed Stage 1 gates, and commits. Do not run adoption, disk, ingest, builds, or other gates; never synthesize their receipts.' },
  missing_mode_a: { stage: '2', slug: 'mode-a', next: 'missing_conventions', gate_ids: ['mode_a_targeted_tests', 'mode_a_smoke'], contract: 'Run the exact targeted Mode A gates first. If the already-present implementation passes, make no pricing implementation change and write only bootstrap/mode-a.json. Implement/fix Mode A only when an exact targeted gate proves it necessary. Do not implement/stub Mode B; never benchmark holdout.' },
  missing_conventions: { stage: '3', slug: 'conventions', next: 'missing_mode_b', gate_ids: ['convention_tests', 'mode_a_smoke_tune', 'convention_speed_measure', 'residual_floor', 'convention_speed'], contract: 'Resolve conventions on aggregate smoke+tune Mode A with the closed staged sweep, commit CONVENTIONS.md + iter-000 + exact v2 bootstrap/conventions.json including BOTH 11-metric floor arrays (metrics/baseline_metrics/metric_deltas and symmetric_metrics/baseline_symmetric_metrics/symmetric_metric_deltas), the map/blob OIDs and the rel-avx2 pin. The COMMITTED RATCHET BASELINE is the symmetric array, and the hard no-regression gate is stated against it: the symmetric loss |m-o|/max(|m|,|o|,floor) is what the scale selection minimises, it is bounded and has no smallest-scale gradient, so gate and selector optimise one objective. The standard-relative array |m-o|/max(|o|,floor) is committed unchanged beside it purely so the floor stays directly comparable to the charter target of greeks within 1% rel; it is validated for shape, population parity and delta arithmetic but is never the regression criterion, and the two must not be unified. The gate order is executable as listed: convention_speed_measure runs BEFORE residual_floor because residual_floor hard-requires the committed iter-000 while convention_speed_measure is the only sanctioned producer of the rel-avx2 rows_per_second measurement iter-000 needs and must therefore precede it. Derive iter-000 speed from that receipt, never copy it verbatim: baseline = the measured rows_per_second and pin = floor(baseline * 0.90). convention_speed then re-measures on a quiet host against the committed pin. Never benchmark holdout, read Mode B, or edit Ratchet memory; PM updates memory only after audited landing.' },
  missing_mode_b: { stage: '4', slug: 'mode-b', next: 'ready', gate_ids: ['mode_b_targeted_tests', 'mode_b_smoke_tune'], contract: 'Implement/test Mode B, run aggregate smoke+tune, commit bootstrap/mode-b.json. Never change holdout/conventions or benchmark holdout.' },
}

function validSuccessEvidence(evidence) {
  return Array.isArray(evidence) && evidence.length > 0 && evidence.every(item => item && typeof item.command === 'string' && item.command.trim() && !iterationCommandError(item.command) && item.exit_code === 0 && typeof item.output === 'string' && item.output.trim())
}

function iterationCommandError(command) {
  const text = String(command || '').trim()
  if (!text) return 'empty command'
  if (/[;&|`]/.test(text)) return 'chained command forbidden in oracle loop'
  if (/\s+\((?:cwd|working directory)\s+[^)]*\)\s*$/i.test(text)) return 'annotated command forbidden in oracle loop'
  if (/(?:^|\s)-L(?:\s|=)/i.test(text) || /run_all_gates/i.test(text)) return 'label/full regression command forbidden in oracle loop'
  if (/(?:^|\s)(?:ctest|-Ctest)(?:\s|$)/i.test(text)) {
    const match = text.match(/(?:^|\s)-R\s+(\S+)/i)
    if (!match || !/^\^[A-Za-z][A-Za-z0-9_.-]{3,63}\$$/.test(match[1]) || /atx_vol/i.test(match[1])) return 'broad or unanchored ctest forbidden in oracle loop'
  }
  const tokens = text.split(/\s+/)
  for (let index = 0; index < tokens.length; index += 1) {
    if (/(?:^|[\\/])[^\\/\s]*(?:test|tests)(?:\.exe)?$/i.test(tokens[index]) && !/^-/i.test(tokens[index]) &&
        !['build', '--target'].includes(String(tokens[index - 1] || '').toLowerCase())) return 'direct test executable forbidden in oracle loop'
  }
  if (/atx-build\.ps1[^\r\n]*\bbuild(?:\s*$|\s+all(?:\s|$))/i.test(text) || /(?:--target|\bbuild\s+)\s+all(?:\s|$)/i.test(text)) return 'broad build target forbidden in oracle loop'
  if (/\bpytest\b/i.test(text) || /\bnode\s+--test\b/i.test(text) || /cmake\s+--build/i.test(text) && !/--target\s+[A-Za-z0-9_.+-]+/i.test(text)) return 'broad runner/build forbidden in oracle loop'
  return null
}

function bootstrapCommandError(command) {
  const policyError = iterationCommandError(command)
  if (policyError) return policyError
  if (/[\x00-\x1f\x7f]/.test(String(command || ''))) return 'control character forbidden in bootstrap command'
  if (/[()]/.test(String(command || ''))) return 'parenthesized annotation forbidden in bootstrap command'
  return null
}

function validBootstrapSuccessEvidence(evidence, required) {
  return Array.isArray(evidence) && (!required || evidence.length > 0) && evidence.every(item =>
    item && typeof item.command === 'string' && item.command.trim() && !bootstrapCommandError(item.command) &&
    item.exit_code === 0 && typeof item.output === 'string' && item.output.trim())
}

function exactEvidenceSet(evidence, commands) {
  return validSuccessEvidence(evidence) && evidence.length === commands.length && commands.every(command => evidence.filter(item => item.command === command).length === 1) &&
    evidence.every(item => !iterationCommandError(item.command))
}

function diagnosticsUseForbiddenCommand(diagnostics) {
  return Array.isArray(diagnostics) && diagnostics.some(item => iterationCommandError(item && item.command))
}

function sha256HexUtf8(value) {
  const bytes = []
  for (const symbol of String(value)) {
    const code = symbol.codePointAt(0)
    if (code <= 0x7f) bytes.push(code)
    else if (code <= 0x7ff) bytes.push(0xc0 | (code >>> 6), 0x80 | (code & 0x3f))
    else if (code <= 0xffff) bytes.push(0xe0 | (code >>> 12), 0x80 | ((code >>> 6) & 0x3f), 0x80 | (code & 0x3f))
    else bytes.push(0xf0 | (code >>> 18), 0x80 | ((code >>> 12) & 0x3f), 0x80 | ((code >>> 6) & 0x3f), 0x80 | (code & 0x3f))
  }
  const bitLength = bytes.length * 8
  bytes.push(0x80)
  while (bytes.length % 64 !== 56) bytes.push(0)
  const high = Math.floor(bitLength / 0x100000000)
  const low = bitLength >>> 0
  for (let shift = 24; shift >= 0; shift -= 8) bytes.push((high >>> shift) & 0xff)
  for (let shift = 24; shift >= 0; shift -= 8) bytes.push((low >>> shift) & 0xff)
  const k = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
  ]
  const h = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]
  const rotate = (word, bits) => (word >>> bits) | (word << (32 - bits))
  for (let offset = 0; offset < bytes.length; offset += 64) {
    const w = new Array(64)
    for (let index = 0; index < 16; index += 1) {
      const cursor = offset + index * 4
      w[index] = ((bytes[cursor] << 24) | (bytes[cursor + 1] << 16) | (bytes[cursor + 2] << 8) | bytes[cursor + 3]) >>> 0
    }
    for (let index = 16; index < 64; index += 1) {
      const s0 = rotate(w[index - 15], 7) ^ rotate(w[index - 15], 18) ^ (w[index - 15] >>> 3)
      const s1 = rotate(w[index - 2], 17) ^ rotate(w[index - 2], 19) ^ (w[index - 2] >>> 10)
      w[index] = (w[index - 16] + s0 + w[index - 7] + s1) >>> 0
    }
    let [a, b, c, d, e, f, g, hh] = h
    for (let index = 0; index < 64; index += 1) {
      const s1 = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)
      const choice = (e & f) ^ (~e & g)
      const first = (hh + s1 + choice + k[index] + w[index]) >>> 0
      const s0 = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)
      const majority = (a & b) ^ (a & c) ^ (b & c)
      const second = (s0 + majority) >>> 0
      hh = g; g = f; f = e; e = (d + first) >>> 0; d = c; c = b; b = a; a = (first + second) >>> 0
    }
    const chunk = [a, b, c, d, e, f, g, hh]
    for (let index = 0; index < h.length; index += 1) h[index] = (h[index] + chunk[index]) >>> 0
  }
  return h.map(word => word.toString(16).padStart(8, '0')).join('')
}

function brokerGateOutputSha256(output) {
  return sha256HexUtf8(String(output))
}

function brokerGateReceiptId(operationId, receipt) {
  return sha256HexUtf8(JSON.stringify({
    operation_id: operationId,
    gate_id: receipt.gate_id,
    tested_sha: receipt.tested_sha,
    tested_tree: receipt.tested_tree,
    command: receipt.command,
    exit_code: receipt.exit_code,
    raw_output_sha256: receipt.broker_evidence.raw_output_sha256,
  }))
}

function validBootstrapDiagnostics(diagnostics, required) {
  if (diagnostics === undefined) return !required
  return Array.isArray(diagnostics) && (!required || diagnostics.length > 0) && diagnostics.every(item =>
    item && typeof item.command === 'string' && item.command.trim() && !bootstrapCommandError(item.command) &&
    Number.isInteger(item.exit_code) && typeof item.output === 'string' && item.output.trim())
}

function validLeaseReceipt(receipt, expected, action) {
  if (!receipt || receipt.action !== action || receipt.exit_code !== 0 || !String(receipt.output || '').trim()) return false
  if (receipt.lease_name !== expected.lease_name || receipt.run_id !== expected.run_id || receipt.branch !== expected.branch || receipt.base_sha !== expected.base_sha ||
      receipt.worktree !== expected.worktree || receipt.heartbeat_id !== expected.heartbeat_id || receipt.keeper_pid !== expected.keeper_pid ||
      receipt.keeper_process_started_utc !== expected.keeper_process_started_utc) return false
  if (!Number.isInteger(receipt.keeper_pid) || receipt.keeper_pid <= 0 || !/^\d{4}-/.test(receipt.keeper_process_started_utc || '') || !/^\d{4}-/.test(receipt.keeper_ready_utc || '')) return false
  return receipt.output.includes(receipt.lease_name) && receipt.output.includes(receipt.run_id) &&
    (action !== 'acquire' || (receipt.output.includes(String(receipt.keeper_pid)) && receipt.output.includes(receipt.keeper_ready_utc) &&
      receipt.output.includes(` heartbeat_id=${receipt.heartbeat_id}`)))
}

function validBrokerEvidence(receipt, logicalOperation = null, physicalCwd = null, allowCanonical = false, allowFailure = false) {
  if (!receipt || !Number.isInteger(receipt.exit_code) || (!allowFailure && receipt.exit_code !== 0) || typeof receipt.command !== 'string' || !receipt.command.trim() || typeof receipt.output !== 'string' ||
      !/^[0-9a-f]{64}$/.test(receipt.raw_output_sha256 || '') || (logicalOperation && receipt.logical_operation !== logicalOperation)) return false
  if (physicalCwd && String(receipt.physical_cwd || '').replace(/\//g, '\\').toLowerCase() !== String(physicalCwd).replace(/\//g, '\\').toLowerCase()) return false
  const before = receipt.root_guard_before; const after = receipt.root_guard_after
  if (!before || !after || !/^[0-9a-f]{40}$/.test(before.main_sha || '') || before.main_sha !== after.main_sha) return false
  for (const key of ['index_sha256', 'tracked_sha256', 'untracked_sha256', 'raw_sha256']) if (!/^[0-9a-f]{64}$/.test(before[key] || '') || !/^[0-9a-f]{64}$/.test(after[key] || '')) return false
  for (const key of ['index_sha256', 'tracked_sha256', 'untracked_sha256']) if (before[key] !== after[key]) return false
  return allowCanonical || before.canonical_sha === after.canonical_sha
}

function brokerAcquireError(acquire, expected) {
  if (!acquire || !/^[0-9a-f]{64}$/.test(acquire.capability || '') || acquire.operation_id !== expected.operation_id || acquire.stage !== expected.stage ||
      acquire.run_id !== expected.run_id || acquire.branch !== expected.branch || acquire.base_sha !== expected.base_sha || acquire.heartbeat_id !== expected.heartbeat_id) return 'broker acquisition identity invalid'
  if (!/^[0-9a-f]{40}$/.test(acquire.lease_start_sha || '') || typeof acquire.recovery_replay !== 'boolean' ||
      (acquire.recovery_replay ? expected.operation_id !== 'bootstrap_data' : acquire.lease_start_sha !== acquire.base_sha)) return 'broker acquisition replay identity invalid'
  if (!/^pool-[0-9]+$/.test(acquire.lease_name || '') || !/[\\/]atx-wt[\\/]pool-[0-9]+$/i.test(acquire.worktree || '') ||
      !acquire.worktree.replace(/\//g, '\\').toLowerCase().endsWith(`\\${acquire.lease_name.toLowerCase()}`)) return 'broker acquisition path invalid'
  if (!validLeaseReceipt(acquire.acquisition_receipt, { ...expected, lease_name: acquire.lease_name, worktree: acquire.worktree, keeper_pid: acquire.keeper_pid, keeper_process_started_utc: acquire.keeper_process_started_utc }, 'acquire')) return 'broker acquisition receipt invalid'
  return validBrokerEvidence(acquire.broker_evidence, 'lane_open') ? null : 'broker acquisition root evidence invalid'
}

function brokerReleaseError(release, acquire, finalizeExpected) {
  if (!release || !release.released || release.lease_name !== acquire.lease_name || !/^[0-9a-f]{40}$/.test(release.sha || '') || !/^[0-9a-f]{40}$/.test(release.tree || '') ||
      (finalizeExpected ? !/^[0-9a-f]{64}$/.test(release.finalize_capability || '') : release.finalize_capability !== '')) return 'broker release identity invalid'
  if (!validLeaseReceipt(release.release_receipt, { ...acquire, run_id: acquire.run_id }, 'release')) return 'broker release receipt invalid'
  return validBrokerEvidence(release.broker_evidence, 'lane_release') ? null : 'broker release root evidence invalid'
}

function brokerQuarantineError(quarantine, acquire) {
  if (!quarantine || !quarantine.quarantined || quarantine.lease_name !== acquire.lease_name || !/^[0-9a-f]{40}$/.test(quarantine.sha || '') ||
      !/^[0-9a-f]{40}$/.test(quarantine.tree || '') || !Array.isArray(quarantine.preserved_paths) || !Array.isArray(quarantine.gate_receipts)) return 'broker quarantine identity invalid'
  if (quarantine.gate_receipts.length) {
    const gateIds = BOOTSTRAP_LANES.missing_data.gate_ids
    if (quarantine.gate_receipts.length !== gateIds.length || !quarantine.gate_receipts.some(receipt => receipt.exit_code !== 0) ||
        new Set(quarantine.gate_receipts.map(receipt => receipt.receipt_id)).size !== gateIds.length) return 'broker quarantine gate evidence invalid'
    for (const gateId of gateIds) {
      const matches = quarantine.gate_receipts.filter(receipt => receipt.gate_id === gateId)
      const receipt = matches[0]
      if (matches.length !== 1 || receipt.tested_sha !== quarantine.sha || receipt.tested_tree !== quarantine.tree || receipt.command !== BOOTSTRAP_GATE_COMMANDS[gateId] ||
          !validBrokerEvidence(receipt.broker_evidence, `gate:${gateId}`, acquire.worktree, false, true) || receipt.broker_evidence.exit_code !== receipt.exit_code || receipt.broker_evidence.command !== receipt.command ||
          receipt.broker_evidence.output !== receipt.output) return `broker quarantine gate evidence invalid: ${gateId}`
    }
  }
  if (!validLeaseReceipt(quarantine.quarantine_receipt, { ...acquire, run_id: acquire.run_id }, 'quarantine')) return 'broker quarantine receipt invalid'
  return validBrokerEvidence(quarantine.broker_evidence, 'lane_quarantine') ? null : 'broker quarantine root evidence invalid'
}

function validHeadReceipt(receipt, ref, sha, tree = null) {
  return !!receipt && receipt.ref === ref && receipt.sha === sha && (!tree || receipt.tree === tree) && receipt.exit_code === 0 && receipt.command.trim() === `git rev-parse ${ref}` && receipt.output.trim() === sha
}

// PowerShell 5.1's ConvertFrom-Json parses JSON numbers into System.Decimal and
// re-emits the literal source digits, while C++ prints %.17g and JS prints the
// shortest round-trip. A byte comparison of the two renderings therefore fails
// for most MAE-shaped doubles, and a Stage 3 receipt carries ~75 of them. Both
// sides are canonicalised through JS number formatting instead. Key ORDER, the
// property this check exists to bind, is preserved: JSON.parse keeps the source
// order and JSON.stringify replays it.
function sameJsonPayload(text, value) {
  if (typeof text !== 'string') return false
  try { return JSON.stringify(JSON.parse(text)) === JSON.stringify(value) } catch { return false }
}

function validGateReceipt(receipt, gateId, expectedSha, expectedTree) {
  if (!receipt || receipt.gate_id !== gateId || receipt.command !== BOOTSTRAP_GATE_COMMANDS[gateId] || receipt.exit_code !== 0) return false
  if (expectedSha !== undefined && (!/^[0-9a-f]{64}$/.test(receipt.receipt_id || '') || receipt.tested_sha !== expectedSha || receipt.tested_tree !== expectedTree ||
      !validBrokerEvidence(receipt.broker_evidence, `gate:${gateId}`) || receipt.broker_evidence.command !== receipt.command || receipt.broker_evidence.output !== receipt.output ||
      receipt.broker_evidence.exit_code !== receipt.exit_code)) return false
  const result = receipt.result
  if (!result || result.schema_version !== 1 || result.status !== 'PASS' || result.command_id !== gateId ||
      !Number.isInteger(result.observations) || result.observations <= 0 || !/^[0-9a-f]{64}$/.test(result.raw_output_sha256 || '') ||
      !sameJsonPayload(receipt.output, result)) return false
  const commonKeys = ['schema_version', 'status', 'observations', 'command_id', 'raw_output_sha256']
  if (!TARGETED_BOOTSTRAP_GATE_IDS.includes(gateId)) return Object.keys(result).sort().join(',') === commonKeys.sort().join(',')
  const semanticKeys = [...commonKeys, 'tested_sha', 'tested_tree', 'gate_kind', 'tests_executed', 'tests_passed', 'rows_processed', 'metric_ids', 'audit_summary']
  if (['mode_a_smoke_tune', 'residual_floor'].includes(gateId)) semanticKeys.push('rows_total', 'engine_errors', 'metrics', 'baseline_metrics', 'metric_deltas', 'symmetric_metrics', 'baseline_symmetric_metrics', 'symmetric_metric_deltas', 'conventions', 'production_conventions', 'candidate_prices', 'input_model_regressed_greeks', 'diagnostic_speed')
  if (['convention_speed_measure', 'convention_speed'].includes(gateId)) semanticKeys.push('speed')
  if (Object.keys(result).sort().join(',') !== semanticKeys.sort().join(',')) return false
  if (!/^[0-9a-f]{40}$/.test(result.tested_sha || '') || !/^[0-9a-f]{40}$/.test(result.tested_tree || '') ||
      (expectedSha !== undefined && (result.tested_sha !== expectedSha || result.tested_tree !== expectedTree))) return false
  if (gateId.endsWith('_tests')) return result.gate_kind === 'ctest' && Number.isInteger(result.tests_executed) && result.tests_executed > 0 &&
    (gateId !== 'mode_a_targeted_tests' || result.tests_executed === ORACLE_BENCH_TEST_COUNT) &&
    (gateId !== 'convention_tests' || result.tests_executed === ORACLE_CONVENTION_TEST_COUNT) &&
    result.tests_passed === result.tests_executed && result.rows_processed === 0 && Array.isArray(result.metric_ids) && result.metric_ids.length === 0 &&
    result.audit_summary === `tests_executed=${result.tests_executed} tests_passed=${result.tests_passed}`
  const wanted = expectedBootstrapMetricIds(gateId)
  const wantedKind = gateId === 'mode_a_smoke_tune' ? 'oracle_convention' : gateId === 'residual_floor' ? 'oracle_floor_verify'
    : ['convention_speed_measure', 'convention_speed'].includes(gateId) ? 'oracle_speed' : 'oracle_bench'
  if (result.gate_kind !== wantedKind || result.tests_executed !== 0 || result.tests_passed !== 0 || !Number.isInteger(result.rows_processed) || result.rows_processed <= 0 ||
      !Array.isArray(result.metric_ids) || result.metric_ids.length !== wanted.length || new Set(result.metric_ids).size !== wanted.length || !wanted.every(id => result.metric_ids.includes(id))) return false
  const auditPrefix = `status=PASS rows_processed=${result.rows_processed} metric_ids=${[...result.metric_ids].sort().join(',')}`
  if (['mode_a_smoke_tune', 'residual_floor'].includes(gateId)) {
    if (!validStage3MetricArray(result.metrics, wanted) || !validStage3MetricArray(result.baseline_metrics, wanted) || !validStage3ConventionMap(result.conventions) ||
        !validStage3MetricArray(result.symmetric_metrics, wanted) || !validStage3MetricArray(result.baseline_symmetric_metrics, wanted) ||
        !validStage3ConventionMap(result.production_conventions) ||
        !Array.isArray(result.metric_deltas) || result.metric_deltas.length !== wanted.length ||
        !Array.isArray(result.symmetric_metric_deltas) || result.symmetric_metric_deltas.length !== wanted.length || !Array.isArray(result.candidate_prices) || result.candidate_prices.length !== 8 ||
        !result.diagnostic_speed || result.diagnostic_speed.preset !== 'dev' || result.diagnostic_speed.citable !== false || !(result.diagnostic_speed.rows_per_second > 0) || !(result.diagnostic_speed.wall_seconds > 0)) return false
    // Row accounting must close: a sweep that lost 99% of its rows to engine
    // errors would otherwise report PASS on the 1% it managed to price.
    if (!Number.isInteger(result.rows_total) || !Number.isInteger(result.engine_errors) || result.engine_errors < 0 ||
        result.rows_total !== result.rows_processed + result.engine_errors) return false
    // The weakest selection ratio is carried in the audit line, so a scale
    // chosen on a sliver of the cohort is visible without opening the artifact.
    if (result.audit_summary !== `${auditPrefix} min_selection_pct=${minSelectionPercent(result.metrics)}`) return false
    // Fail closed while the map production prices with differs from the map the
    // sweep resolved: a committed floor must never describe an unused map.
    if (JSON.stringify(result.production_conventions) !== JSON.stringify(result.conventions)) return false
    // One row population per metric across both arms AND both objectives, or the
    // delta arrays compare two different samples. The symmetric array is a
    // second OBJECTIVE over the same rows, never a second population.
    const baselineById = new Map(result.baseline_metrics.map(item => [item.metric_id, item]))
    const symmetricById = new Map(result.symmetric_metrics.map(item => [item.metric_id, item]))
    const baselineSymmetricById = new Map(result.baseline_symmetric_metrics.map(item => [item.metric_id, item]))
    if (result.metrics.some(item => {
      const baseline = baselineById.get(item.metric_id)
      const symmetric = symmetricById.get(item.metric_id)
      const baselineSymmetric = baselineSymmetricById.get(item.metric_id)
      return !baseline || baseline.count !== item.count || baseline.selection_count !== item.selection_count ||
        !symmetric || symmetric.count !== item.count || symmetric.selection_count !== item.selection_count ||
        !baselineSymmetric || baselineSymmetric.count !== item.count || baselineSymmetric.selection_count !== item.selection_count
    })) return false
    // HARD no-regression gate, on the SYMMETRIC array: no symmetric metric may be
    // worse than its baseline. Equality is allowed because mode_a_vol_mae is
    // structurally 0 on both arms. The symmetric loss is what the scale selection
    // minimises — bounded, no smallest-scale gradient — so gate and selector
    // optimise one objective; the standard-relative array pins its denominator on
    // near-zero-oracle rows and would systematically reward the smaller
    // multiplier, contradicting the selector instead of catching a defect. It is
    // still validated for shape/parity/delta arithmetic and published unchanged so
    // the floor stays comparable to the charter target. No bypass flag, no
    // allowlist, no tolerance — a map that makes the selected number worse than
    // doing nothing is not a candidate.
    if (result.symmetric_metrics.some(item => item.value > (baselineSymmetricById.get(item.metric_id) || {}).value)) return false
    // Greeks the SELECTED input model still regresses on: a unique subset of the
    // nine relative Greek ids (price and vol are absolute floors, not part of
    // the input-model Greek comparison).
    const greekIds = wanted.filter(id => id !== 'mode_a_price_mae' && id !== 'mode_a_vol_mae')
    const regressed = result.input_model_regressed_greeks
    if (!Array.isArray(regressed) || new Set(regressed).size !== regressed.length ||
        regressed.some(id => !greekIds.includes(id))) return false
    // Both delta arrays get the identical check against their OWN metric array:
    // coverage, finiteness, positive count, `candidate - baseline == delta` to
    // 1e-12, and a count equal to that array's reported population.
    for (const [deltas, byId] of [[result.metric_deltas, new Map(result.metrics.map(item => [item.metric_id, item]))],
                                  [result.symmetric_metric_deltas, symmetricById]]) {
      const deltaIds = new Set(deltas.map(item => item && item.metric_id))
      if (deltaIds.size !== wanted.length || !wanted.every(id => deltaIds.has(id)) || deltas.some(item => !item || !Number.isFinite(item.candidate) || !Number.isFinite(item.baseline) || !Number.isFinite(item.delta) || !Number.isInteger(item.count) || item.count <= 0 || Math.abs((item.candidate - item.baseline) - item.delta) > 1e-12 || item.count !== (byId.get(item.metric_id) || {}).count)) return false
    }
  } else if (result.audit_summary !== auditPrefix) return false
  if (['convention_speed_measure', 'convention_speed'].includes(gateId)) {
    const speed = result.speed
    if (!speed || speed.metric_id !== SPEED_METRIC_ID || speed.unit !== 'rows_per_second' || speed.preset !== 'rel-avx2' || speed.quiet_host !== true || !Number.isFinite(speed.value) || speed.value <= 0 || !Number.isInteger(speed.count) || speed.count <= 0) return false
    const speedKeys = ['metric_id', 'value', 'count', 'unit', 'preset', 'quiet_host']
    if (gateId === 'convention_speed_measure') {
      // The measure arm pins NOTHING: it exists precisely because no pin can
      // exist yet, so a pin here would mean it validated against something.
      if (Object.keys(speed).sort().join(',') !== speedKeys.sort().join(',')) return false
    } else if (Object.keys(speed).sort().join(',') !== [...speedKeys, 'pin'].sort().join(',') ||
        !Number.isFinite(speed.pin) || speed.pin <= 0 || speed.value < speed.pin) return false
  }
  return true
}

function validStage3MetricArray(metrics, wanted) {
  if (!Array.isArray(metrics) || metrics.length !== wanted.length || new Set(metrics.map(item => item && item.metric_id)).size !== wanted.length) return false
  // selection_count > 0 alone would admit a production scale chosen on a handful
  // of the aggregate rows; require at least a tenth of the reported population.
  return wanted.every(id => metrics.some(item => item && item.metric_id === id && Number.isFinite(item.value) && item.value >= 0 && Number.isInteger(item.count) && item.count > 0 &&
    Number.isInteger(item.selection_count) && item.selection_count > 0 && item.selection_count <= item.count && 10 * item.selection_count >= item.count &&
    item.unit === (id === 'mode_a_price_mae' ? 'ticks' : id === 'mode_a_vol_mae' ? 'bp' : 'relative')))
}

// Weakest per-metric selection ratio, as an integer percent. The same
// floor(100 * selection_count / count) IEEE arithmetic runs in
// scripts/oracle-targeted-gate.ps1, so the two layers agree exactly.
function minSelectionPercent(metrics) {
  return metrics.reduce((low, item) => Math.min(low, Math.floor((100 * item.selection_count) / item.count)), 100)
}

function validStage3ConventionMap(map) {
  if (!map || typeof map !== 'object' || Array.isArray(map)) return false
  const wanted = CONVENTION_MAP.required
  return Object.keys(map).length === wanted.length && wanted.every(key => typeof map[key] === 'string' && map[key].length > 0)
}

function expectedBootstrapMetricIds(gateId) {
  if (['mode_a_smoke', 'mode_a_smoke_tune', 'residual_floor'].includes(gateId)) return TARGET_REGISTRY.filter(item => item.mode === 'A').map(item => item.metric_id)
  if (['convention_speed_measure', 'convention_speed'].includes(gateId)) return [SPEED_METRIC_ID]
  if (gateId === 'mode_b_smoke_tune') return TARGET_REGISTRY.filter(item => item.mode === 'B').map(item => item.metric_id)
  return []
}

function expectedGateMetricIds(gateId) {
  if (gateId === 'holdout_mode_a') return [...TARGET_REGISTRY, ...AGGREGATE_REGISTRY].filter(item => item.mode === 'A').map(item => item.metric_id)
  if (gateId === 'holdout_mode_b') return [...TARGET_REGISTRY, ...AGGREGATE_REGISTRY].filter(item => item.mode === 'B').map(item => item.metric_id)
  if (gateId === 'rel_avx2_speed') return [SPEED_METRIC_ID]
  return []
}

function expectedMeasureMetricIds(gateId) {
  if (gateId === 'measure_mode_a') return [...TARGET_REGISTRY.filter(item => item.mode === 'A').map(item => item.metric_id), 'mode_a_aggregate_error']
  if (gateId === 'measure_mode_b') return [...TARGET_REGISTRY.filter(item => item.mode === 'B').map(item => item.metric_id), 'mode_b_aggregate_error']
  if (gateId === 'measure_speed') return [SPEED_METRIC_ID]
  return []
}

function expectedMetricUnit(metricId) {
  if (metricId === SPEED_METRIC_ID) return 'rows_per_second'
  return [...TARGET_REGISTRY, ...AGGREGATE_REGISTRY].find(item => item.metric_id === metricId)?.unit || null
}

function validNumericMetric(metric, metricId, requirePin) {
  if (!metric || metric.metric_id !== metricId || metric.unit !== expectedMetricUnit(metricId) || !Number.isFinite(metric.value) || metric.value < 0 ||
      !Number.isInteger(metric.count) || metric.count <= 0) return false
  const keys = Object.keys(metric).sort().join(',')
  if (requirePin) return keys === 'count,metric_id,pin,unit,value' && Number.isFinite(metric.pin) && metric.pin > 0 && metric.value >= metric.pin
  return keys === 'count,metric_id,unit,value'
}

function validMeasureGateReceipt(receipt, gateId) {
  if (!receipt || receipt.gate_id !== gateId || receipt.command !== READY_MEASURE_GATES[gateId] || receipt.exit_code !== 0) return false
  const result = receipt.result
  const wanted = expectedMeasureMetricIds(gateId)
  if (!result || result.schema_version !== 1 || result.status !== 'PASS' || result.command_id !== gateId ||
      !Number.isInteger(result.observations) || result.observations <= 0 || !Array.isArray(result.metrics) ||
      result.metrics.length !== wanted.length || new Set(result.metrics.map(metric => metric && metric.metric_id)).size !== wanted.length ||
      !wanted.every(metricId => validNumericMetric(result.metrics.find(metric => metric && metric.metric_id === metricId), metricId, metricId === SPEED_METRIC_ID))) return false
  return sameJsonPayload(receipt.output, result)
}

function measurePayloadError(measure) {
  if (!measure || Object.prototype.hasOwnProperty.call(measure, 'attribution_payload')) return 'Measure may not self-report baseline payload'
  if (!Array.isArray(measure.gate_receipts) || measure.gate_receipts.length !== Object.keys(READY_MEASURE_GATES).length) return 'Measure gate receipt set mismatch'
  for (const gateId of Object.keys(READY_MEASURE_GATES)) {
    const matches = measure.gate_receipts.filter(receipt => receipt && receipt.gate_id === gateId)
    if (matches.length !== 1 || !validMeasureGateReceipt(matches[0], gateId)) return `Measure gate receipt invalid: ${gateId}`
  }
  const context = measure.analysis_context
  const candidate = { schema_version: 2, iteration: Number(String(measure.iter || '').replace(/^iter-/, '')), target_metrics: [], aggregate_metrics: [], speed: null,
    prior_refuted_ids: context && context.prior_refuted_ids, oracle_suspect_cells: context && context.oracle_suspect_cells, conventions: context && context.conventions }
  return aggregatePayloadError(attributionPayloadFromMeasure(measure, candidate))
}

function attributionPayloadFromMeasure(measure, candidate = null) {
  const byMetric = new Map((measure.gate_receipts || []).flatMap(receipt => receipt?.result?.metrics || []).map(metric => [metric.metric_id, metric]))
  const context = measure.analysis_context || {}
  const payload = candidate || { schema_version: 2, iteration: Number(String(measure.iter || '').replace(/^iter-/, '')),
    prior_refuted_ids: context.prior_refuted_ids, oracle_suspect_cells: context.oracle_suspect_cells, conventions: context.conventions }
  payload.target_metrics = TARGET_REGISTRY.map(item => { const metric = byMetric.get(item.metric_id); return { metric_id: item.metric_id, mode: item.mode, baseline: metric?.value, count: metric?.count, unit: item.unit } })
  payload.aggregate_metrics = AGGREGATE_REGISTRY.map(item => { const metric = byMetric.get(item.metric_id); return { metric_id: item.metric_id, mode: item.mode, baseline: metric?.value, count: metric?.count, unit: item.unit } })
  const speed = byMetric.get(SPEED_METRIC_ID)
  payload.speed = { metric_id: SPEED_METRIC_ID, baseline: speed?.value, pin: speed?.pin, unit: 'rows_per_second' }
  return payload
}

function validRatchetGateReceipt(receipt, gateId) {
  if (!receipt || receipt.gate_id !== gateId || receipt.command !== RATCHET_GATE_COMMANDS[gateId] || receipt.exit_code !== 0 || !String(receipt.output || '').trim()) return false
  const result = receipt.result
  const wanted = expectedGateMetricIds(gateId)
  return !!result && result.schema_version === 1 && result.status === 'PASS' && result.command_id === gateId && Number.isInteger(result.observations) && result.observations > 0 &&
    Array.isArray(result.metrics) && result.metrics.length === wanted.length && new Set(result.metrics.map(metric => metric && metric.metric_id)).size === wanted.length &&
    wanted.every(id => validNumericMetric(result.metrics.find(metric => metric && metric.metric_id === id), id, false)) &&
    sameJsonPayload(receipt.output, result)
}

function validIntegrationCommand(receipt, reviewedSha, reviewedTree = null) {
  return !!receipt && receipt.reviewed_sha === reviewedSha && (!reviewedTree || (receipt.reviewed_tree === reviewedTree && receipt.tree_after === reviewedTree)) && receipt.exit_code === 0 &&
    new RegExp(`^git\\s+(?:merge(?:\\s+--(?:ff-only|no-ff|no-edit))*|cherry-pick(?:\\s+--(?:ff|no-commit))*)\\s+${reviewedSha}$`, 'i').test(String(receipt.command || '').trim())
}

function casReceiptError(receipt, expected) {
  if (!receipt) return 'CAS receipt missing'
  if (receipt.ref !== expected.ref || receipt.new_sha !== expected.new_sha || receipt.new_tree !== expected.new_tree || receipt.expected_old_sha !== expected.expected_old_sha) return 'CAS identity mismatch'
  if (receipt.command.trim() !== `git update-ref ${expected.ref} ${expected.new_sha} ${expected.expected_old_sha}` || receipt.exit_code !== 0 || !String(receipt.output || '').trim() ||
      !validBrokerEvidence(receipt.broker_evidence, 'canonical_finalize', null, true)) return 'CAS command receipt invalid'
  return null
}

function auditReceiptError(receipt, ref) {
  if (!receipt) return 'canonical audit missing'
  if (receipt.ref !== ref || receipt.command.trim() !== `git rev-parse ${ref}` ||
      (!/^[0-9a-f]{40}$/i.test(receipt.sha || '') && receipt.sha !== 'MISSING') || receipt.output.trim() !== receipt.sha ||
      !validBrokerEvidence(receipt.broker_evidence, 'canonical_audit')) return 'canonical audit invalid'
  if ((receipt.sha === 'MISSING' && receipt.exit_code === 0) || (receipt.sha !== 'MISSING' && receipt.exit_code !== 0)) return 'canonical audit invalid'
  return null
}

function reviewContractError(review, expectedSha) {
  if (!review) return 'missing review'
  if (!['APPROVE', 'BLOCK'].includes(review.verdict) || !Array.isArray(review.findings)) return 'review shape invalid'
  if (review.reviewed_sha !== expectedSha) return 'reviewed SHA mismatch'
  if (!validSuccessEvidence(review.evidence) || diagnosticsUseForbiddenCommand(review.diagnostics) || !Array.isArray(review.broker_evidence) ||
      !review.broker_evidence.length || !review.broker_evidence.every(item => validBrokerEvidence(item))) return 'review has invalid/broad broker evidence'
  const blockers = review.findings.filter(finding => finding.severity === 'blocker')
  if (review.verdict === 'APPROVE' && blockers.length) return 'APPROVE contains blocker'
  if (review.verdict === 'BLOCK' && !blockers.length) return 'BLOCK lacks blocker'
  return null
}

function validAdoptionReceipt(receipt, baseSha) {
  if (!receipt || receipt.command !== ADOPTION_COMMAND || receipt.exit_code !== 0 || !receipt.result || receipt.output !== JSON.stringify(receipt.result)) return false
  const result = receipt.result
  const keys = Object.keys(result).sort().join(',')
  if (result.schema_version !== 1 || result.command_id !== 'oracle_existing_store_adoption') return false
  if (result.status === 'ADOPTED') {
    if (keys !== ['base_sha', 'bucket_count', 'cohort_underlier_count', 'command_id', 'holdout_membership_sha256', 'manifest_sha256', 'parquet_files', 'schema_version', 'status', 'total_rows'].sort().join(',')) return false
    return result.base_sha === baseSha && /^[0-9a-f]{64}$/.test(result.manifest_sha256 || '') && /^[0-9a-f]{64}$/.test(result.holdout_membership_sha256 || '') &&
      Number.isInteger(result.total_rows) && result.total_rows > 0 && Number.isInteger(result.bucket_count) && result.bucket_count > 0 &&
      Number.isInteger(result.parquet_files) && result.parquet_files > 0 && Number.isInteger(result.cohort_underlier_count) && result.cohort_underlier_count > 0
  }
  const reasons = ['cohort_schema_smoke', 'cohort_schema_tune', 'cohort_schema_holdout', 'cohort_disjointness', 'cohort_dates', 'manifest_missing', 'store_validation', 'manifest_validation', 'cohort_store_coverage', 'holdout_digest', 'holdout_digest_mismatch', 'cohort_blob', 'publication_transaction', 'publication_recovery', 'validation_exception']
  return result.status === 'INGEST_REQUIRED' && keys === ['command_id', 'reason', 'schema_version', 'status'].sort().join(',') && reasons.includes(result.reason)
}

function validChangedPathReceipt(receipt, report) {
  if (!receipt || receipt.base_sha !== report.base_sha || receipt.tested_sha !== report.sha || receipt.exit_code !== 0) return false
  if (receipt.command !== `git diff --name-only ${report.base_sha}...${report.sha}` || !Array.isArray(receipt.paths) || !receipt.paths.length) return false
  if (new Set(receipt.paths).size !== receipt.paths.length || receipt.paths.some(path => !/^[A-Za-z0-9._/-]+$/.test(path) || path.startsWith('/') || path.includes('..'))) return false
  const sorted = [...receipt.paths].sort()
  return receipt.paths.every((path, index) => path === sorted[index]) && receipt.output.trim() === receipt.paths.join('\n')
}

function validPrecheckGateReceipt(receipt, gateId, expected, worktree) {
  if (!receipt || receipt.gate_id !== gateId || receipt.command !== BOOTSTRAP_GATE_COMMANDS[gateId] || !String(receipt.output || '').trim() ||
      !/^[0-9a-f]{64}$/.test(receipt.receipt_id || '') || receipt.tested_sha !== expected.base_sha || receipt.tested_tree !== expected.base_tree ||
      !validBrokerEvidence(receipt.broker_evidence, `gate:${gateId}`, worktree, false, true) || receipt.broker_evidence.command !== receipt.command ||
      receipt.broker_evidence.output !== receipt.output || receipt.broker_evidence.exit_code !== receipt.exit_code ||
      receipt.broker_evidence.raw_output_sha256 !== brokerGateOutputSha256(receipt.output) ||
      receipt.receipt_id !== brokerGateReceiptId(expected.operation_id, receipt)) return false
  const keys = Object.keys(receipt).sort().join(',')
  const outerKeys = ['receipt_id', 'gate_id', 'tested_sha', 'tested_tree', 'command', 'status', 'exit_code', 'output', 'broker_evidence']
  if (receipt.status === 'FAIL') return Number.isInteger(receipt.exit_code) && receipt.exit_code !== 0 && keys === outerKeys.sort().join(',')
  if (receipt.status !== 'PASS' || receipt.exit_code !== 0 || keys !== [...outerKeys, 'result'].sort().join(',')) return false
  return validGateReceipt(receipt, gateId, expected.base_sha, expected.base_tree)
}

function sealedRecoveryQueryError(query, acquire, expected) {
  if (!query || !query.found || !/^[0-9a-f]{64}$/.test(query.recovery_id || '') || !query.result || query.result.recovery_id !== query.recovery_id ||
      !validBrokerEvidence(query.broker_evidence, 'recovery_result', acquire.worktree)) return 'sealed recovery query missing/invalid'
  const result = query.result
  if (!result.replayed || !/^[0-9a-f]{40}$/.test(result.sha || '') || !/^[0-9a-f]{40}$/.test(result.tree || '') ||
      !Array.isArray(result.broker_evidence) || !result.broker_evidence.length || !result.broker_evidence.every(item => validBrokerEvidence(item, 'recover_stage1_replay', acquire.worktree))) return 'sealed recovery replay identity invalid'
  const source = result.recovery
  if (!source || source.source_commit !== STAGE1_RECOVERY.source_commit || source.source_parent !== STAGE1_RECOVERY.source_parent ||
      source.source_tree !== STAGE1_RECOVERY.source_tree || source.adoption_rerun !== false || !Array.isArray(source.blobs)) return 'sealed recovery source invalid'
  const paths = Object.keys(STAGE1_RECOVERY.blobs).sort()
  if (JSON.stringify([...(result.replay_paths || [])].sort()) !== JSON.stringify(paths) || JSON.stringify([...(result.files_changed || [])].sort()) !== JSON.stringify(paths) ||
      JSON.stringify([...(result.changed_path_receipt?.paths || [])].sort()) !== JSON.stringify(paths) || result.changed_path_receipt?.base_sha !== expected.base_sha || result.changed_path_receipt?.tested_sha !== result.sha) return 'sealed recovery path closure invalid'
  for (const path of paths) {
    const proof = source.blobs.filter(item => item.path === path && item.source_blob === STAGE1_RECOVERY.blobs[path] && item.replay_blob === STAGE1_RECOVERY.blobs[path])
    if (proof.length !== 1) return `sealed recovery blob invalid: ${path}`
  }
  const gateIds = BOOTSTRAP_LANES.missing_data.gate_ids
  if (!Array.isArray(result.gate_receipts) || result.gate_receipts.length !== gateIds.length || new Set(result.gate_receipts.map(receipt => receipt.receipt_id)).size !== gateIds.length) return 'sealed recovery gate set invalid'
  for (const gateId of gateIds) {
    const matches = result.gate_receipts.filter(receipt => receipt.gate_id === gateId)
    if (matches.length !== 1) return `sealed recovery gate missing: ${gateId}`
    const receipt = matches[0]
    let parsed
    try { parsed = JSON.parse(receipt.output) } catch { return `sealed recovery gate output invalid: ${gateId}` }
    if (!/^[0-9a-f]{64}$/.test(receipt.receipt_id || '') || receipt.tested_sha !== result.sha || receipt.tested_tree !== result.tree ||
        !validGateReceipt({ gate_id: gateId, command: receipt.command, exit_code: receipt.exit_code, output: receipt.output, result: parsed }, gateId) ||
        !validBrokerEvidence(receipt.broker_evidence, `gate:${gateId}`) || receipt.broker_evidence.command !== receipt.command || receipt.broker_evidence.output !== receipt.output) return `sealed recovery gate invalid: ${gateId}`
  }
  return null
}

function reportFromSealedRecovery(query, acquire, expected) {
  const result = query.result
  return {
    state: 'missing_data', outcome: 'DONE', branch: expected.branch, sha: result.sha, tree: result.tree, base_sha: expected.base_sha,
    worktree: acquire.worktree, lease_name: acquire.lease_name, lease_run_id: expected.run_id, heartbeat_id: expected.heartbeat_id,
    keeper_pid: acquire.keeper_pid, keeper_process_started_utc: acquire.keeper_process_started_utc, acquisition_receipt: acquire.acquisition_receipt,
    holdout_digest_receipt: STAGE1_RECOVERY.holdout_digest, bootstrap_path: 'data_recovery', recovery_id: query.recovery_id, recovery_replayed: true,
    recovery_source_receipt: result.recovery, changed_path_receipt: result.changed_path_receipt,
    evidence: result.gate_receipts.map(receipt => ({ command: receipt.command, exit_code: receipt.exit_code, output: receipt.output })),
    broker_evidence: result.broker_evidence, deviations: 'sealed broker recovery result replayed after worker response loss',
  }
}

function bootstrapPathError(report, expected) {
  if (!['missing_data', 'missing_mode_a'].includes(expected.state)) return null
  if (!validChangedPathReceipt(report.changed_path_receipt, report)) return 'bootstrap changed-path receipt invalid'
  const paths = report.changed_path_receipt.paths
  if (expected.state === 'missing_data') {
    if (report.bootstrap_path !== 'data_recovery' || Object.prototype.hasOwnProperty.call(report, 'adoption_receipt') || Object.prototype.hasOwnProperty.call(report, 'disk_receipt')) return 'Stage1 must use recovery without adoption/disk'
    if (!/^[0-9a-f]{64}$/.test(report.recovery_id || '') || typeof report.recovery_replayed !== 'boolean') return 'Stage1 sealed recovery identity invalid'
    const recovery = report.recovery_source_receipt
    if (!recovery || recovery.source_commit !== STAGE1_RECOVERY.source_commit || recovery.source_parent !== STAGE1_RECOVERY.source_parent ||
        recovery.source_tree !== STAGE1_RECOVERY.source_tree || recovery.adoption_rerun !== false || !Array.isArray(recovery.blobs)) return 'Stage1 recovery source identity invalid'
    const wanted = Object.keys(STAGE1_RECOVERY.blobs).sort()
    if (paths.length !== wanted.length || !wanted.every((path, index) => paths[index] === path) || recovery.blobs.length !== wanted.length) return 'Stage1 recovery path set invalid'
    for (const path of wanted) {
      const matches = recovery.blobs.filter(item => item && item.path === path && item.source_blob === STAGE1_RECOVERY.blobs[path] && item.replay_blob === STAGE1_RECOVERY.blobs[path])
      if (matches.length !== 1) return `Stage1 recovery blob invalid: ${path}`
    }
    if (report.holdout_digest_receipt !== STAGE1_RECOVERY.holdout_digest) return 'Stage1 recovery digest mismatch'
    const forbidden = report.evidence.filter(item => item.command === ADOPTION_COMMAND || item.command === BOOTSTRAP_GATE_COMMANDS.disk || /oracle_ingest|build|ctest/i.test(item.command || ''))
    if (forbidden.length) return 'Stage1 recovery includes adoption/disk/ingest/build evidence'
    for (const gateId of BOOTSTRAP_LANES.missing_data.gate_ids) if (report.evidence.filter(item => item.command === BOOTSTRAP_GATE_COMMANDS[gateId] && item.exit_code === 0).length !== 1) return `Stage1 recovery evidence missing: ${gateId}`
    return null
  }
  if (Object.prototype.hasOwnProperty.call(report, 'adoption_receipt') || Object.prototype.hasOwnProperty.call(report, 'disk_receipt')) return 'Stage2 contains Stage1 receipt'
  if (!Array.isArray(report.precheck_gate_receipts) || report.precheck_gate_receipts.length !== 2) return 'Stage2 precheck receipt set invalid'
  if (new Set(report.precheck_gate_receipts.map(receipt => receipt && receipt.receipt_id)).size !== 2) return 'Stage2 precheck receipt IDs duplicated'
  const gateIds = ['mode_a_targeted_tests', 'mode_a_smoke']
  for (const gateId of gateIds) {
    const matches = report.precheck_gate_receipts.filter(receipt => receipt && receipt.gate_id === gateId)
    if (matches.length !== 1 || !validPrecheckGateReceipt(matches[0], gateId, expected, report.worktree)) return `Stage2 precheck receipt invalid: ${gateId}`
  }
  const existingPasses = report.precheck_gate_receipts.every(receipt => receipt.status === 'PASS')
  if (existingPasses) return report.bootstrap_path === 'mode_a_receipt_only' && paths.length === MODE_A_RECEIPT_ONLY_PATHS.length && MODE_A_RECEIPT_ONLY_PATHS.every((path, index) => paths[index] === path) ? null : 'Stage2 passing implementation must be receipt-only'
  return report.bootstrap_path === 'mode_a_implementation' && paths.includes(MODE_A_RECEIPT_ONLY_PATHS[0]) && paths.length > 1 ? null : 'Stage2 failed precheck requires implementation path'
}

function bootstrapLeaseIdentityError(report, expected) {
  if (!report || typeof report !== 'object' || Array.isArray(report)) return 'bootstrap build incomplete'
  const nonempty = ['state', 'branch', 'base_sha', 'worktree', 'lease_name', 'lease_run_id', 'heartbeat_id', 'keeper_process_started_utc']
  if (nonempty.some(key => typeof report[key] !== 'string' || !report[key].trim())) return 'bootstrap lease identity field empty'
  if (!/^[0-9a-f]{40}$/.test(report.base_sha) || report.state !== expected.state || report.branch !== expected.branch ||
      report.base_sha !== expected.base_sha || report.lease_run_id !== expected.run_id || report.heartbeat_id !== expected.heartbeat_id) return 'bootstrap lease identity mismatch'
  if (!Number.isInteger(report.keeper_pid) || report.keeper_pid <= 0 || !/^\d{4}-/.test(report.keeper_process_started_utc)) return 'bootstrap keeper identity invalid'
  if (!/^pool-[0-9]+$/.test(report.lease_name) || !/[\\/]atx-wt[\\/]pool-[0-9]+$/i.test(report.worktree) ||
      !report.worktree.replace(/\//g, '\\').toLowerCase().endsWith(`\\${report.lease_name.toLowerCase()}`)) return 'bootstrap build not isolated'
  if (!validLeaseReceipt(report.acquisition_receipt, {
    lease_name: report.lease_name, run_id: expected.run_id, branch: expected.branch, base_sha: expected.base_sha, worktree: report.worktree,
    heartbeat_id: expected.heartbeat_id, keeper_pid: report.keeper_pid, keeper_process_started_utc: report.keeper_process_started_utc,
  }, 'acquire')) return 'bootstrap acquisition receipt invalid'
  return null
}

function unwrapBootstrapReport(envelope) {
  if (!envelope || typeof envelope !== 'object' || Array.isArray(envelope)) return { report: null, error: 'bootstrap StructuredOutput envelope invalid' }
  const keys = Object.keys(envelope)
  if (keys.length !== 1 || keys[0] !== 'report' || !envelope.report || typeof envelope.report !== 'object' || Array.isArray(envelope.report)) {
    return { report: null, error: 'bootstrap StructuredOutput envelope invalid' }
  }
  return { report: envelope.report, error: null }
}

function bootstrapReportError(report, expected) {
  const leaseError = bootstrapLeaseIdentityError(report, expected)
  if (leaseError) return leaseError
  if (typeof report.deviations !== 'string' || !Array.isArray(report.broker_evidence) || !report.broker_evidence.length ||
      !report.broker_evidence.every(item => validBrokerEvidence(item, null, report.worktree))) return 'bootstrap broker evidence invalid'
  if (report.outcome === 'BLOCKED') {
    if ((report.sha && !/^[0-9a-f]{40}$/.test(report.sha)) || (report.tree && !/^[0-9a-f]{40}$/.test(report.tree)) || (!!report.sha !== !!report.tree) ||
        (report.holdout_digest_receipt && !/^[0-9a-f]{64}$/.test(report.holdout_digest_receipt))) return 'blocked bootstrap SHA/receipt invalid'
    if (!Array.isArray(report.blockers) || !report.blockers.length || report.blockers.some(item => typeof item !== 'string' || !item.trim())) return 'blocked bootstrap blockers missing'
    if (!validBootstrapSuccessEvidence(report.evidence, false)) return 'blocked bootstrap success evidence invalid'
    if (!validBootstrapDiagnostics(report.diagnostics, true)) return 'blocked bootstrap diagnostics invalid'
    return `bootstrap blocked: ${report.blockers.join('; ')}`
  }
  if (report.outcome !== 'DONE') return 'bootstrap build incomplete'
  if (!/^[0-9a-f]{40}$/.test(report.sha || '') || !/^[0-9a-f]{40}$/.test(report.tree || '') || !/^[0-9a-f]{64}$/.test(report.holdout_digest_receipt || '') ||
      (expected.holdout_digest_receipt && report.holdout_digest_receipt !== expected.holdout_digest_receipt) || !validBootstrapSuccessEvidence(report.evidence, true) ||
      !validBootstrapDiagnostics(report.diagnostics, false)) return 'bootstrap build evidence/SHA/receipt invalid'
  if (report.bootstrap_path !== undefined && !['data_recovery', 'data_ingest', 'mode_a_receipt_only', 'mode_a_implementation', 'standard'].includes(report.bootstrap_path)) return 'bootstrap path invalid'
  const pathError = bootstrapPathError(report, expected)
  if (pathError) return pathError
  return null
}

function bootstrapPrepareError(report, review, prepare, expected) {
  const buildError = bootstrapReportError(report, expected)
  if (buildError) return buildError
  const reviewError = reviewContractError(review, report.sha)
  if (reviewError || review.verdict !== 'APPROVE') return reviewError || 'bootstrap not approved'
  if (!prepare || !prepare.passed || diagnosticsUseForbiddenCommand(prepare.diagnostics) ||
      !Array.isArray(prepare.broker_evidence) || !prepare.broker_evidence.length || !prepare.broker_evidence.every(item => validBrokerEvidence(item, null, prepare.integration_worktree))) return 'scoped verifier missing/failed'
  if (prepare.reviewed_sha !== report.sha || prepare.reviewed_tree !== report.tree || prepare.integration_sha !== report.sha || prepare.integration_tree !== report.tree || prepare.integration_branch !== expected.integration_branch ||
      prepare.lease_run_id !== expected.run_id || prepare.next_state !== expected.next_state || prepare.holdout_digest_receipt !== report.holdout_digest_receipt) return 'bootstrap prepare identity/state mismatch'
  if (!/^pool-[0-9]+$/.test(prepare.integration_lease || '') || !/[\\/]atx-wt[\\/]pool-[0-9]+$/i.test(prepare.integration_worktree || '') ||
      !prepare.integration_worktree.replace(/\//g, '\\').toLowerCase().endsWith(`\\${prepare.integration_lease.toLowerCase()}`)) return 'bootstrap verifier not isolated'
  const laneLease = { lease_name: report.lease_name, run_id: expected.run_id, branch: expected.branch, base_sha: expected.base_sha, worktree: report.worktree, heartbeat_id: expected.heartbeat_id, keeper_pid: report.keeper_pid, keeper_process_started_utc: report.keeper_process_started_utc }
  const integrationLease = { lease_name: prepare.integration_lease, run_id: expected.run_id, branch: expected.integration_branch, base_sha: expected.base_sha, worktree: prepare.integration_worktree, heartbeat_id: expected.integration_heartbeat_id, keeper_pid: prepare.keeper_pid, keeper_process_started_utc: prepare.keeper_process_started_utc }
  if (!validLeaseReceipt(prepare.lane_release_receipt, laneLease, 'release')) return 'bootstrap lane release receipt invalid'
  if (!validLeaseReceipt(prepare.acquisition_receipt, integrationLease, 'acquire')) return 'bootstrap integration acquisition receipt invalid'
  if (!validLeaseReceipt(prepare.integration_release_receipt, integrationLease, 'release')) return 'bootstrap integration release receipt invalid'
  if (!validIntegrationCommand(prepare.integration_receipt, report.sha, report.tree) || prepare.integration_receipt.head_after !== report.sha ||
      !String(prepare.integration_receipt.output || '').includes(report.sha)) return 'exact reviewed SHA integration receipt invalid'
  if (!validHeadReceipt(prepare.head_receipt, 'HEAD', report.sha, report.tree)) return 'bootstrap HEAD/tree receipt invalid'
  if (!Array.isArray(prepare.gate_receipts) || prepare.gate_receipts.length !== expected.gate_ids.length) return 'bootstrap gate receipt set mismatch'
  for (const gateId of expected.gate_ids) {
    const matches = prepare.gate_receipts.filter(receipt => receipt && receipt.gate_id === gateId)
    if (matches.length !== 1 || !validGateReceipt(matches[0], gateId, report.sha, report.tree)) return `bootstrap required gate receipt invalid: ${gateId}`
  }
  if (new Set(prepare.gate_receipts.map(receipt => receipt.receipt_id)).size !== prepare.gate_receipts.length) return 'bootstrap gate receipt IDs are duplicated'
  return null
}

function aggregatePayloadError(payload) {
  if (!payload || typeof payload !== 'object' || Array.isArray(payload)) return 'aggregate payload missing/not object'
  const exactKeys = (value, keys) => { const actual = Object.keys(value).sort(); const wanted = [...keys].sort(); return actual.length === wanted.length && actual.every((key, index) => key === wanted[index]) }
  const payloadKeys = ['schema_version', 'iteration', 'target_metrics', 'aggregate_metrics', 'speed', 'prior_refuted_ids', 'oracle_suspect_cells', 'conventions']
  if (!exactKeys(payload, payloadKeys) || payload.schema_version !== 2 || !Number.isInteger(payload.iteration) || payload.iteration < 0) return 'aggregate payload keys/version invalid'
  for (const values of [payload.prior_refuted_ids, payload.oracle_suspect_cells]) {
    if (!Array.isArray(values) || values.length > 256 || new Set(values).size !== values.length ||
        !values.every(value => Number.isSafeInteger(value) && value >= 0 && value <= 2147483647)) return 'aggregate payload numeric ID list invalid'
  }
  const conventionKeys = CONVENTION_MAP.required
  const conventions = payload.conventions
  if (!conventions || typeof conventions !== 'object' || Array.isArray(conventions) || !exactKeys(conventions, conventionKeys) ||
      !validStage3ConventionMap(conventions)) return 'aggregate convention map invalid'
  const baselineKeys = ['metric_id', 'mode', 'baseline', 'count', 'unit']
  const validateRegistry = (metrics, registry) => {
    if (!Array.isArray(metrics) || metrics.length !== registry.length) return false
    const byId = new Map(metrics.map(metric => [metric && metric.metric_id, metric]))
    if (byId.size !== registry.length) return false
    return registry.every(expected => {
      const metric = byId.get(expected.metric_id)
      return metric && typeof metric === 'object' && !Array.isArray(metric) && exactKeys(metric, baselineKeys) && metric.mode === expected.mode &&
        metric.unit === expected.unit && Number.isFinite(metric.baseline) && metric.baseline >= 0 && Number.isInteger(metric.count) && metric.count > 0
    })
  }
  if (!validateRegistry(payload.target_metrics, TARGET_REGISTRY) || !validateRegistry(payload.aggregate_metrics, AGGREGATE_REGISTRY)) return 'aggregate metric registry invalid'
  if (!payload.speed || typeof payload.speed !== 'object' || Array.isArray(payload.speed) ||
      !exactKeys(payload.speed, ['metric_id', 'baseline', 'pin', 'unit']) || payload.speed.metric_id !== SPEED_METRIC_ID ||
      payload.speed.unit !== 'rows_per_second' || !Number.isFinite(payload.speed.baseline) || !Number.isFinite(payload.speed.pin) ||
      payload.speed.pin <= 0 || payload.speed.baseline < payload.speed.pin) return 'aggregate speed baseline invalid'
  return null
}

function metricDeltaConsistent(metric) {
  if (!metric || ![metric.baseline, metric.candidate, metric.delta, metric.pin].every(Number.isFinite)) return false
  const expected = metric.candidate - metric.baseline
  const scale = Math.max(1, Math.abs(metric.baseline), Math.abs(metric.candidate), Math.abs(metric.delta))
  return Math.abs(metric.delta - expected) <= Number.EPSILON * scale * 16
}

function expectedRatchetMetrics(payload) {
  const targetBaselines = new Map(payload.target_metrics.map(metric => [metric.metric_id, metric.baseline]))
  const aggregateBaselines = new Map(payload.aggregate_metrics.map(metric => [metric.metric_id, metric.baseline]))
  return [
    ...TARGET_REGISTRY.map(item => ({ metric_id: item.metric_id, mode: item.mode, unit: item.unit, gate: 'target', direction: 'lower', baseline: targetBaselines.get(item.metric_id), pin: item.limit })),
    ...AGGREGATE_REGISTRY.map(item => ({ ...item, gate: 'aggregate', direction: 'lower', baseline: aggregateBaselines.get(item.metric_id), pin: 0 })),
    { metric_id: SPEED_METRIC_ID, mode: 'ALL', unit: 'rows_per_second', gate: 'speed', direction: 'higher', baseline: payload.speed.baseline, pin: payload.speed.pin },
  ]
}

function sameNumber(left, right) {
  if (!Number.isFinite(left) || !Number.isFinite(right)) return false
  return Math.abs(left - right) <= Number.EPSILON * Math.max(1, Math.abs(left), Math.abs(right)) * 16
}

function distanceToInterval(value, low, high) {
  if (value < low) return low - value
  if (value > high) return value - high
  return 0
}

function marketCommand(cellId) {
  return `atx-vol-oracle-bench --cohort holdout --market-check ${cellId} --aggregate-only`
}

function marketReceiptError(receipt, cellId) {
  if (!receipt || receipt.cell_id !== cellId || receipt.command !== marketCommand(cellId) || receipt.exit_code !== 0 || !String(receipt.output || '').trim()) return 'identity/command invalid'
  const numbers = [receipt.nbbo_bid_mean, receipt.nbbo_ask_mean, receipt.atx_price_mean, receipt.oracle_price_mean, receipt.atx_nbbo_distance, receipt.oracle_nbbo_distance]
  if (!numbers.every(Number.isFinite) || receipt.nbbo_bid_mean > receipt.nbbo_ask_mean || !Number.isInteger(receipt.sample_count) || receipt.sample_count <= 0) return 'typed NBBO fields invalid'
  const atxDistance = distanceToInterval(receipt.atx_price_mean, receipt.nbbo_bid_mean, receipt.nbbo_ask_mean)
  const oracleDistance = distanceToInterval(receipt.oracle_price_mean, receipt.nbbo_bid_mean, receipt.nbbo_ask_mean)
  if (!sameNumber(receipt.atx_nbbo_distance, atxDistance) || !sameNumber(receipt.oracle_nbbo_distance, oracleDistance) || !(atxDistance < oracleDistance)) return 'market does not mechanically side with atx-vol'
  const expectedOutput = JSON.stringify({
    cell_id: receipt.cell_id, nbbo_bid_mean: receipt.nbbo_bid_mean, nbbo_ask_mean: receipt.nbbo_ask_mean,
    atx_price_mean: receipt.atx_price_mean, oracle_price_mean: receipt.oracle_price_mean,
    atx_nbbo_distance: receipt.atx_nbbo_distance, oracle_nbbo_distance: receipt.oracle_nbbo_distance, sample_count: receipt.sample_count,
  })
  if (receipt.output !== expectedOutput) return 'market output does not bind typed fields'
  return null
}

function relativeRegression(metric) {
  const denominator = Math.max(Math.abs(metric.baseline), 1e-15)
  return metric.direction === 'lower' ? (metric.candidate - metric.baseline) / denominator : (metric.baseline - metric.candidate) / denominator
}

function ratchetGateIdForMetric(metricId) {
  if (metricId === SPEED_METRIC_ID) return 'rel_avx2_speed'
  const target = TARGET_REGISTRY.find(item => item.metric_id === metricId)
  const aggregate = AGGREGATE_REGISTRY.find(item => item.metric_id === metricId)
  const mode = (target || aggregate)?.mode
  return mode === 'A' ? 'holdout_mode_a' : mode === 'B' ? 'holdout_mode_b' : null
}

function ratchetCandidateValue(ratchet, metricId) {
  const gateId = ratchetGateIdForMetric(metricId)
  const receipt = (ratchet.gate_receipts || []).find(item => item && item.gate_id === gateId)
  return receipt?.result?.metrics?.find(metric => metric && metric.metric_id === metricId)?.value
}

function ratchetMetricsFromReceipts(ratchet) {
  return (ratchet.metrics || []).map(metric => {
    const candidate = ratchetCandidateValue(ratchet, metric.metric_id)
    return { ...metric, candidate, delta: candidate - metric.baseline }
  })
}

function ratchetPrepareContractError(ratchet, expected) {
  if (!ratchet || !validSuccessEvidence(ratchet.evidence) || diagnosticsUseForbiddenCommand(ratchet.diagnostics)) return 'Ratchet evidence missing/failed'
  if (ratchet.tested_sha !== expected.tested_sha || ratchet.tested_branch !== expected.tested_branch || ratchet.base_sha !== expected.tested_sha ||
      ratchet.ratchet_branch !== expected.ratchet_branch || ratchet.lease_run_id !== expected.run_id || ratchet.heartbeat_id !== expected.heartbeat_id ||
      !/^[0-9a-f]{40}$/i.test(ratchet.ratchet_sha || '')) return 'Ratchet exact integration/base identity mismatch'
  if (!/^pool-[0-9]+$/.test(ratchet.lease_name || '') || !/[\\/]atx-wt[\\/]pool-[0-9]+$/i.test(ratchet.worktree || '') ||
      !ratchet.worktree.replace(/\//g, '\\').toLowerCase().endsWith(`\\${ratchet.lease_name.toLowerCase()}`)) return 'Ratchet lease/worktree invalid'
  const lease = { lease_name: ratchet.lease_name, run_id: expected.run_id, branch: expected.ratchet_branch, base_sha: expected.tested_sha, worktree: ratchet.worktree, heartbeat_id: expected.heartbeat_id, keeper_pid: ratchet.keeper_pid, keeper_process_started_utc: ratchet.keeper_process_started_utc }
  if (!validLeaseReceipt(ratchet.acquisition_receipt, lease, 'acquire')) return 'Ratchet acquisition receipt invalid'
  if (!validLeaseReceipt(ratchet.release_receipt, lease, 'release')) return 'Ratchet release receipt invalid'
  const digest = ratchet.digest_receipt
  if (!digest || digest.expected_digest !== expected.holdout_digest || digest.actual_digest !== expected.holdout_digest || digest.exit_code !== 0 ||
      !/holdout/i.test(digest.command || '') || !/digest/i.test(digest.command || '') || !String(digest.output || '').includes(expected.holdout_digest)) return 'Ratchet digest recomputation receipt invalid'
  if (!Array.isArray(ratchet.applicable_modes) || ratchet.applicable_modes.length !== expected.applicable_modes.length || !expected.applicable_modes.every(mode => ratchet.applicable_modes.includes(mode))) return 'Ratchet applicable modes mismatch'
  if (!Array.isArray(ratchet.metrics) || !ratchet.metrics.length || !Array.isArray(ratchet.metric_evidence)) return 'Ratchet typed metrics missing'
  const expectedMetrics = expectedRatchetMetrics(expected.baseline_contract)
  if (ratchet.metrics.length !== expectedMetrics.length) return 'Ratchet metric registry incomplete/extra'
  const metricById = new Map(ratchet.metrics.map(metric => [metric && metric.metric_id, metric]))
  if (metricById.size !== expectedMetrics.length) return 'Ratchet metric registry duplicated'
  for (const registryMetric of expectedMetrics) {
    const metric = metricById.get(registryMetric.metric_id)
    if (!metric || metric.mode !== registryMetric.mode || metric.gate !== registryMetric.gate || metric.direction !== registryMetric.direction ||
        metric.unit !== registryMetric.unit || !sameNumber(metric.baseline, registryMetric.baseline) || !sameNumber(metric.pin, registryMetric.pin)) return `Ratchet workflow-owned metric contract mismatch: ${registryMetric.metric_id}`
    const measuredCandidate = ratchetCandidateValue(ratchet, registryMetric.metric_id)
    if (!Number.isFinite(measuredCandidate) || !sameNumber(metric.candidate, measuredCandidate)) return `Ratchet candidate is not bound to typed gate output: ${registryMetric.metric_id}`
    if (!metricDeltaConsistent(metric)) return 'Ratchet metric delta inconsistent'
    if (!Number.isInteger(metric.evidence_index) || metric.evidence_index < 0 || metric.evidence_index >= ratchet.metric_evidence.length) return 'Ratchet metric shape/evidence index invalid'
    const evidence = ratchet.metric_evidence[metric.evidence_index]
    if (!evidence || evidence.exit_code !== 0 || !/holdout|speed/i.test(evidence.command || '') || !String(evidence.output || '').includes(metric.metric_id) ||
        !String(evidence.output || '').includes(String(metric.baseline)) || !String(evidence.output || '').includes(String(metric.candidate)) ||
        !String(evidence.output || '').includes(String(metric.delta))) return 'Ratchet metric not supported by referenced output'
  }
  if (!Array.isArray(ratchet.gate_receipts) || ratchet.gate_receipts.length !== RATCHET_GATE_IDS.length) return 'Ratchet gate receipt set mismatch'
  for (const gateId of RATCHET_GATE_IDS) {
    const matches = ratchet.gate_receipts.filter(receipt => receipt && receipt.gate_id === gateId)
    if (matches.length !== 1 || !validRatchetGateReceipt(matches[0], gateId)) return `Ratchet required gate receipt invalid: ${gateId}`
  }
  if (!Array.isArray(ratchet.oracle_suspects_excluded) || !Array.isArray(ratchet.market_evidence) || ratchet.market_evidence.length !== ratchet.oracle_suspects_excluded.length) return 'Ratchet suspect market evidence missing'
  if (new Set(ratchet.oracle_suspects_excluded).size !== ratchet.oracle_suspects_excluded.length ||
      !ratchet.oracle_suspects_excluded.every(cell => expected.suspect_candidates.includes(cell))) return 'Ratchet suspect exclusion not frozen by Measure'
  for (const cell of ratchet.oracle_suspects_excluded) {
    const matches = ratchet.market_evidence.filter(receipt => receipt && receipt.cell_id === cell)
    if (matches.length !== 1) return `Ratchet suspect evidence invalid: ${cell}`
    const marketError = marketReceiptError(matches[0], cell)
    if (marketError) return `Ratchet suspect evidence invalid: ${cell}: ${marketError}`
  }
  if (!ratchet.northstar_updated || !Array.isArray(ratchet.ledger_appended) || !ratchet.ledger_appended.length || typeof ratchet.holdout_summary !== 'string' || !ratchet.holdout_summary.trim()) return 'Ratchet memory/summary incomplete'
  return null
}

function computeRatchetVerdict(ratchet) {
  const measuredMetrics = ratchetMetricsFromReceipts(ratchet)
  const targetPass = measuredMetrics.filter(metric => metric.gate === 'target').every(metric => metric.direction === 'lower' ? metric.candidate < metric.baseline : metric.candidate > metric.baseline)
  const aggregatePass = measuredMetrics.filter(metric => metric.gate === 'aggregate').every(metric => relativeRegression(metric) <= 0.02 + 1e-12)
  const speed = measuredMetrics.find(metric => metric.gate === 'speed')
  const speedPass = speed.direction === 'higher' && speed.candidate >= speed.pin
  return targetPass && aggregatePass && speedPass ? 'ACCEPT' : 'REJECT'
}

phase('Capability')
const capability = await agent(
  'Call the fixed broker capability_probe once with an empty object. Return its typed result unchanged.',
  { agentType: 'vol-capability-inspector', schema: CAPABILITY, label: 'capability' },
)
if (!capability || capability.canonical_ref !== CANONICAL_REF || !/^[0-9a-f]{40}$/i.test(capability.base_sha || '') || !/^[0-9a-f]{40}$/i.test(capability.base_tree || '') || !validSuccessEvidence(capability.evidence) || !validBrokerEvidence(capability.broker_evidence, 'capability_probe')) throw new Error('capability freeze invalid')
if (capability.base_ref !== (capability.canonical_exists ? CANONICAL_REF : REQUESTED_BASE)) throw new Error('capability base-ref selection invalid')
if (capability.evidence.length !== 1 || capability.evidence[0].command !== 'powershell scripts\\oracle-capability.ps1' ||
    !capability.evidence[0].output.includes(`state=${capability.state}`) ||
    !capability.evidence[0].output.includes(`canonical_exists=${String(capability.canonical_exists)}`) ||
    !capability.evidence[0].output.includes(`base_ref=${capability.base_ref}`) ||
    !capability.evidence[0].output.includes(`base_sha=${capability.base_sha}`) ||
    !capability.evidence[0].output.includes(`base_tree=${capability.base_tree}`)) throw new Error('capability probe receipt invalid')
if (capability.state !== 'missing_data' && !/^[0-9a-f]{64}$/i.test(capability.holdout_digest_receipt || '')) throw new Error('holdout digest receipt invalid')
const BASE_SHA = capability.base_sha
const RUN_ID = oracleRunId(BASE_SHA, capability.state, capability.next_iter)
const RUN_SLUG = RUN_ID
const CANONICAL_EXPECTED_OLD = capability.canonical_exists ? BASE_SHA : ZERO_SHA

if (capability.state !== 'ready') {
  const lane = BOOTSTRAP_LANES[capability.state]
  const branch = `lane/oracle-bootstrap-${lane.slug}-${RUN_SLUG}`
  const heartbeat = `${RUN_SLUG}-bootstrap-${lane.slug}`
  const integrationBranch = `integration/oracle-bootstrap-${lane.slug}-${RUN_SLUG}`
  const integrationHeartbeat = `${RUN_SLUG}-bootstrap-integration`
  const operationId = BOOTSTRAP_OPERATION_IDS[capability.state]
  const expected = { state: capability.state, operation_id: operationId, stage: `bootstrap-${lane.stage}`, branch, base_sha: BASE_SHA, base_tree: capability.base_tree, run_id: RUN_ID, heartbeat_id: heartbeat, holdout_digest_receipt: capability.holdout_digest_receipt, integration_branch: integrationBranch, integration_heartbeat_id: integrationHeartbeat, next_state: lane.next, gate_ids: lane.gate_ids }
  phase('Bootstrap Acquire')
  let buildAcquire = null
  let buildAcquireThrown = null
  try {
    buildAcquire = await agent(
      `Call broker lane_open exactly once with ${JSON.stringify({ operation_id: operationId, stage: `bootstrap-${lane.stage}`, run_id: RUN_ID, branch, base_sha: BASE_SHA, heartbeat_id: heartbeat })}; return the broker result unchanged.`,
      { agentType: 'vol-lane-opener', schema: BROKER_ACQUIRE, label: `bootstrap-acquire:${capability.state}` },
    )
  } catch (error) { buildAcquireThrown = String(error) }
  const buildAcquireError = buildAcquireThrown || brokerAcquireError(buildAcquire, expected)
  if (buildAcquireError) return { iteration: `bootstrap-${lane.stage}`, capability_state: capability.state, verdict: 'FAILED', holdout: null, confirmed: [], refuted: [], sprint: null, ledger: [], ratchet_evidence: [], failure: buildAcquireError, bootstrap: { acquire: buildAcquire }, run_id: RUN_ID, base_sha: BASE_SHA, canonical_after: null }
  phase('Bootstrap Build')
  let envelope = null
  let buildThrown = null
  const buildAgentType = capability.state === 'missing_data' ? 'vol-stage1-recovery' : 'vol-builder'
  try {
    envelope = await agent(
      `ONE preleased broker lane; no planner/holdout. Immutable acquisition: ${JSON.stringify(buildAcquire)}. Use only capability=${buildAcquire.capability}. Stage=${lane.stage}. ${lane.contract} Return exactly {report:<typed report>} and copy lease identity from acquisition. Every report evidence item must come from a broker tool result. Include all broker evidence under report.broker_evidence. DONE requires the broker-returned lowercase SHA and tree plus raw lowercase holdout digest. Stage 1 must call only recover_stage1, set bootstrap_path=data_recovery, recovery_id/recovery_replayed exactly from its sealed result, recovery_source_receipt from its recovery field, changed_path_receipt unchanged, sha/tree unchanged, holdout_digest_receipt=${STAGE1_RECOVERY.holdout_digest}, evidence from its four gate receipts, and no adoption_receipt/disk_receipt. Stage 2 must return both complete broker gate receipts unchanged (receipt_id, tested SHA/tree, command, exit, output, broker evidence), add status and parsed result only for PASS, and bind both to workflow base ${BASE_SHA}/${capability.base_tree}; never strip broker fields or synthesize receipts.`,
      { agentType: buildAgentType, schema: BOOTSTRAP_REPORT_TOOL_SCHEMA, label: `bootstrap-build:${capability.state}` },
    )
  } catch (error) { buildThrown = String(error) }
  let unwrapped = unwrapBootstrapReport(envelope)
  let report = unwrapped.report
  let recoveryQuery = null
  let recoveryQueryThrown = null
  if (capability.state === 'missing_data' && (buildThrown || unwrapped.error)) {
    phase('Bootstrap Recovery Result')
    try {
      recoveryQuery = await agent(
        `The Stage 1 worker response was lost or malformed. Call broker recovery_result exactly once with capability=${buildAcquire.capability}; return the broker result unchanged. Do not run recovery or any gate.`,
        { agentType: 'vol-stage1-result-reader', schema: RECOVERY_QUERY, label: 'bootstrap-stage1-recovery-result' },
      )
    } catch (error) { recoveryQueryThrown = String(error) }
    const recoveryQueryError = recoveryQueryThrown || sealedRecoveryQueryError(recoveryQuery, buildAcquire, expected)
    if (!recoveryQueryError) {
      report = reportFromSealedRecovery(recoveryQuery, buildAcquire, expected)
      envelope = { report }
      unwrapped = { report, error: null }
      buildThrown = null
    } else {
      buildThrown = `${buildThrown || unwrapped.error}; sealed recovery unavailable: ${recoveryQueryError}`
    }
  }
  let reportError = buildThrown || unwrapped.error || bootstrapReportError(report, expected)
  let review = null
  if (!reportError) {
    phase('Bootstrap Review')
    try {
      review = await agent(`Fresh exact-SHA broker review ${BASE_SHA}...${report.sha}. Use commit_inspect; Stage 1 must not call generic gate_run because recover_stage1 already owns all four fixed gates atomically. Verify exact recovery source/path closure, SHA/tree ${report.sha}/${report.tree}, and no adoption/ingest/disk. Return command evidence plus every broker evidence unchanged.`, { agentType: 'vol-reviewer', schema: REVIEW, label: `bootstrap-review:${capability.state}` })
      reportError = reviewContractError(review, report.sha)
    } catch (error) { reportError = `bootstrap review failed: ${String(error)}` }
  }
  if (!reportError && review.verdict === 'BLOCK' && capability.state !== 'missing_data') {
    phase('Bootstrap Fix')
    try {
      envelope = await agent(`Fix exactly blockers ${JSON.stringify(review.findings.filter(finding => finding.severity === 'blocker'))} using only workflow-held capability=${buildAcquire.capability}; never acquire/release or select a path. Rerun only gate IDs ${JSON.stringify(lane.gate_ids)}, commit through broker, and return exactly {report:<new typed report>} with broker evidence.`, { agentType: 'vol-builder', schema: BOOTSTRAP_REPORT_TOOL_SCHEMA, label: `bootstrap-fix:${capability.state}` })
    } catch (error) { envelope = null; reportError = `bootstrap Fix failed: ${String(error)}` }
    unwrapped = unwrapBootstrapReport(envelope)
    report = unwrapped.report
    reportError = reportError || unwrapped.error || bootstrapReportError(report, expected)
    if (!reportError) {
      phase('Bootstrap Re-review')
      try {
        review = await agent(`FRESH broker post-Fix review of ${BASE_SHA}...${report.sha} using commit_inspect and capability=${buildAcquire.capability}; never reuse prior verdict.`, { agentType: 'vol-reviewer', schema: REVIEW, label: `bootstrap-rereview:${capability.state}` })
        reportError = reviewContractError(review, report.sha)
      } catch (error) { reportError = `bootstrap re-review failed: ${String(error)}` }
    }
  }
  let buildRelease = null
  let buildReleaseThrown = null
  try {
    buildRelease = await agent(`Call broker lane_release exactly once with capability=${buildAcquire.capability}; return the result unchanged.`, { agentType: 'vol-lane-releaser', schema: BROKER_RELEASE, label: 'bootstrap-build-release' })
  } catch (error) { buildReleaseThrown = String(error) }
  let buildReleaseError = buildReleaseThrown || brokerReleaseError(buildRelease, buildAcquire, false) || (report && /^[0-9a-f]{40}$/.test(report.sha || '') && (buildRelease.sha !== report.sha || buildRelease.tree !== report.tree) ? 'released builder SHA/tree differs from report' : null)
  let buildQuarantine = null
  let buildQuarantineThrown = null
  if (buildReleaseError && capability.state === 'missing_data') {
    try {
      buildQuarantine = await agent(`Clean release failed (${buildReleaseError}). Call broker lane_quarantine exactly once with capability=${buildAcquire.capability}; preserve the dirty lane for audit and return the result unchanged.`, { agentType: 'vol-stage1-quarantiner', schema: BROKER_QUARANTINE, label: 'bootstrap-build-quarantine' })
    } catch (error) { buildQuarantineThrown = String(error) }
    const quarantineError = buildQuarantineThrown || brokerQuarantineError(buildQuarantine, buildAcquire)
    if (!quarantineError) buildReleaseError = 'builder lane quarantined after clean release refusal'
    else buildReleaseError = `${buildReleaseError}; quarantine failed: ${quarantineError}`
  }
  if (reportError || !review || review.verdict !== 'APPROVE') {
    return { iteration: `bootstrap-${lane.stage}`, capability_state: capability.state, verdict: 'FAILED', holdout: null, confirmed: [], refuted: [], sprint: null, ledger: [], ratchet_evidence: [], failure: reportError || buildReleaseError || 'bootstrap not approved', cleanup: buildQuarantine || buildRelease, bootstrap: { acquire: buildAcquire, report, review, release: buildRelease, quarantine: buildQuarantine }, run_id: RUN_ID, base_sha: BASE_SHA, canonical_after: null }
  }
  if (buildReleaseError) return { iteration: `bootstrap-${lane.stage}`, capability_state: capability.state, verdict: 'FAILED', holdout: null, confirmed: [], refuted: [], sprint: null, ledger: [], ratchet_evidence: [], failure: buildReleaseError, bootstrap: { acquire: buildAcquire, report, review, release: buildRelease }, run_id: RUN_ID, base_sha: BASE_SHA, canonical_after: null }
  phase('Bootstrap Integration Acquire')
  const integrationExpected = { operation_id: 'bootstrap_integration', stage: 'bootstrap-prepare', branch: integrationBranch, base_sha: BASE_SHA, run_id: RUN_ID, heartbeat_id: integrationHeartbeat }
  let integrationAcquire = null
  let integrationAcquireThrown = null
  try {
    integrationAcquire = await agent(`Call broker lane_open exactly once with ${JSON.stringify({ operation_id: 'bootstrap_integration', stage: 'bootstrap-prepare', run_id: RUN_ID, branch: integrationBranch, base_sha: BASE_SHA, heartbeat_id: integrationHeartbeat })}; return the result unchanged.`, { agentType: 'vol-lane-opener', schema: BROKER_ACQUIRE, label: `bootstrap-integration-acquire:${capability.state}` })
  } catch (error) { integrationAcquireThrown = String(error) }
  const integrationAcquireError = integrationAcquireThrown || brokerAcquireError(integrationAcquire, integrationExpected)
  if (integrationAcquireError) return { iteration: `bootstrap-${lane.stage}`, capability_state: capability.state, verdict: 'FAILED', holdout: null, confirmed: [], refuted: [], sprint: null, ledger: [], ratchet_evidence: [], failure: integrationAcquireError, bootstrap: { acquire: buildAcquire, report, review, release: buildRelease, integration_acquire: integrationAcquire }, run_id: RUN_ID, base_sha: BASE_SHA, canonical_after: null }
  phase('Bootstrap Verify')
  let verified = null
  let verifyThrown = null
  try {
    verified = await agent(
      `Use only preleased capability=${integrationAcquire.capability}. Call lane_integrate once with reviewed_shas=[${JSON.stringify(report.sha)}], require its reviewed candidate and integrated identities to equal workflow-owned SHA/tree ${report.sha}/${report.tree}, relay exact integration and HEAD receipts, then run each fixed gate ID exactly once: ${JSON.stringify(lane.gate_ids)}. Do not patch, commit, release, or finalize. Return reviewed_tree=${report.tree}, integration_tree=${report.tree}, identities copied from ${JSON.stringify(integrationAcquire)}, holdout_digest_receipt=${report.holdout_digest_receipt}, next_state=${lane.next}. For every gate return the broker receipt_id, tested_sha, tested_tree, command, exit_code, output, parsed typed result, and its own broker_evidence unchanged; all must bind to workflow-owned SHA/tree ${report.sha}/${report.tree}. Include all broker evidence.`,
      { agentType: 'vol-verifier', schema: BOOTSTRAP_VERIFY, label: `bootstrap-verify:${capability.state}` },
    )
  } catch (error) { verifyThrown = String(error) }
  let integrationRelease = null
  let integrationReleaseThrown = null
  try {
    integrationRelease = await agent(`Call broker lane_release exactly once with capability=${integrationAcquire.capability}; return the result unchanged.`, { agentType: 'vol-lane-releaser', schema: BROKER_RELEASE, label: 'bootstrap-integration-release' })
  } catch (error) { integrationReleaseThrown = String(error) }
  const integrationReleaseError = integrationReleaseThrown || brokerReleaseError(integrationRelease, integrationAcquire, true) || (integrationRelease && (integrationRelease.sha !== report.sha || integrationRelease.tree !== report.tree) ? 'integration release differs from workflow-owned reviewed SHA/tree' : null)
  const prepare = verified && { ...verified, lane_release_receipt: buildRelease.release_receipt, acquisition_receipt: integrationAcquire.acquisition_receipt, integration_release_receipt: integrationRelease && integrationRelease.release_receipt }
  const prepareError = bootstrapPrepareError(report, review, prepare, expected)
  if (verifyThrown || integrationReleaseError || prepareError) return { iteration: `bootstrap-${lane.stage}`, capability_state: capability.state, verdict: 'FAILED', holdout: null, confirmed: [], refuted: [], sprint: null, ledger: [], ratchet_evidence: [], failure: verifyThrown || integrationReleaseError || prepareError, bootstrap: { acquire: buildAcquire, report, review, release: buildRelease, integration_acquire: integrationAcquire, verify: verified, integration_release: integrationRelease }, run_id: RUN_ID, base_sha: BASE_SHA, canonical_after: null }
  phase('Bootstrap Finalize')
  const casExpected = { ref: CANONICAL_REF, new_sha: report.sha, new_tree: report.tree, expected_old_sha: CANONICAL_EXPECTED_OLD }
  let finalize = null
  let finalizeThrown = null
  try {
    finalize = await agent(`Call broker canonical_finalize exactly once with finalize_capability=${integrationRelease.finalize_capability}, expected_sha=${report.sha}, expected_tree=${report.tree}; return the typed receipt unchanged.`, { agentType: 'vol-ref-finalizer', schema: CAS_RECEIPT, label: 'bootstrap-cas-finalizer' })
  } catch (error) { finalizeThrown = String(error) }
  const finalizeError = casReceiptError(finalize, casExpected)
  phase('Bootstrap Audit')
  let audit = null
  let auditThrown = null
  try {
    audit = await agent('Call broker canonical_audit exactly once with {}; return its typed result unchanged.', { agentType: 'vol-ref-auditor', schema: HEAD_RECEIPT, label: 'bootstrap-post-cas-audit' })
  } catch (error) { auditThrown = String(error) }
  const auditError = auditThrown || auditReceiptError(audit, CANONICAL_REF)
  const canonicalAfter = auditError || audit.sha === 'MISSING' ? null : audit.sha
  const landed = !finalizeError && !auditError && canonicalAfter === report.sha
  return { iteration: `bootstrap-${lane.stage}`, capability_state: capability.state, verdict: landed ? 'BOOTSTRAP' : 'FAILED', holdout: null, confirmed: [], refuted: [], sprint: null, ledger: [], ratchet_evidence: [], bootstrap: { acquire: buildAcquire, report, review, release: buildRelease, integration_acquire: integrationAcquire, verify: verified, integration_release: integrationRelease, prepare, finalize, audit }, failure: landed ? null : (finalizeThrown || finalizeError || auditError || 'post-CAS canonical mismatch'), landing_status: landed ? 'COMMITTED' : (canonicalAfter === report.sha ? 'LANDED_AUDITED_WITH_INVALID_RECEIPT' : 'NOT_COMMITTED'), run_id: RUN_ID, base_sha: BASE_SHA, canonical_after: canonicalAfter }
}

// Hard cut: the bootstrap/recovery transaction is broker-only.  The ready-state
// Measure/Sprint/Ratchet transaction remains unavailable until every mutation in
// that path has been migrated to the same capability protocol.  Keeping this
// refusal ahead of the retired implementation makes the legacy shell path
// mechanically unreachable.
return {
  iteration: capability.next_iter,
  capability_state: 'ready',
  verdict: 'FAILED',
  holdout: null,
  confirmed: [],
  refuted: [],
  sprint: null,
  ledger: [],
  ratchet_evidence: [],
  failure: 'READY_BROKER_MIGRATION_REQUIRED',
  run_id: RUN_ID,
  base_sha: BASE_SHA,
  canonical_after: BASE_SHA,
}

/* RETIRED_READY_PATH: unreachable until replaced by broker-only transactions.
if (!capability.canonical_exists) throw new Error('ready requires existing canonical ref')
phase('Measure')
const measureBranch = `lane/oracle-measure-${capability.next_iter}-${RUN_SLUG}`
const measureHeartbeat = `${RUN_SLUG}-measure`
const measure = await agent(`Measure ${capability.next_iter} from ${BASE_SHA} in keeper-backed ${measureBranch}, RunId=${RUN_ID}, HeartbeatId=${measureHeartbeat}. Run exactly these three small aggregate commands, once each and no other test/build command: ${JSON.stringify(READY_MEASURE_GATES)}; commit/release. Return one exact typed JSON gate receipt per command with every workflow-registry metric numeric value/count/unit and the speed pin emitted by the pinned benchmark. Return only numeric/enumerated analysis_context; never return an attribution_payload, prose, paths, hashes, membership, rows, or encoded blobs. Return typed acquire/release.`, { schema: MEASURE, label: 'measure' })
const measureLease = measure && { lease_name: measure.lease_name, run_id: RUN_ID, branch: measureBranch, base_sha: BASE_SHA, worktree: measure.worktree, heartbeat_id: measureHeartbeat, keeper_pid: measure.keeper_pid, keeper_process_started_utc: measure.keeper_process_started_utc }
const measureError = !measure || measure.status !== 'ok' || measure.iter !== capability.next_iter || measure.branch !== measureBranch || measure.base_sha !== BASE_SHA || measure.lease_run_id !== RUN_ID || measure.heartbeat_id !== measureHeartbeat || !/^[0-9a-f]{40}$/i.test(measure.sha || '') || !measure.lease_released || !exactEvidenceSet(measure.evidence, READY_MEASURE_COMMANDS) || diagnosticsUseForbiddenCommand(measure.diagnostics) || !validLeaseReceipt(measure.acquisition_receipt, measureLease || {}, 'acquire') || !validLeaseReceipt(measure.release_receipt, measureLease || {}, 'release') || measurePayloadError(measure)
if (measureError) return { iteration: capability.next_iter, capability_state: 'ready', verdict: 'FAILED', holdout: null, confirmed: [], refuted: [], sprint: null, ledger: [], ratchet_evidence: [], failure: 'Measure failed strict contract; no holdout', run_id: RUN_ID, base_sha: BASE_SHA, canonical_after: null }
const attributionPayload = attributionPayloadFromMeasure(measure)

phase('Attribute')
const attribution = await agent(`Tool-less aggregate attribution. Strict payload:\n${JSON.stringify(attributionPayload)}\nRank 1-3 falsifiable hypotheses and reference only registry target_metric_ids. Never request tools, paths, hashes, membership, rows, or encoded data.`, { agentType: 'vol-analyst', schema: ATTR, label: 'attribute' })
if (!attribution || !Array.isArray(attribution.hypotheses) || !attribution.hypotheses.length || attribution.hypotheses.some(item => !Array.isArray(item.target_metric_ids) || !item.target_metric_ids.length || item.target_metric_ids.some(id => !TARGET_REGISTRY.some(target => target.metric_id === id)))) return { iteration: measure.iter, capability_state: 'ready', verdict: 'FAILED', holdout: null, confirmed: [], refuted: [], sprint: null, ledger: [], ratchet_evidence: [], failure: 'Attribution failed; no holdout', run_id: RUN_ID, base_sha: BASE_SHA, canonical_after: null }
const applicableModes = ['A', 'B']

phase('Improve')
let sprint
try { sprint = await workflow('vol-sprint', { task: `Oracle RSI ${measure.iter}; typed aggregate hypotheses:\n${JSON.stringify(attribution.hypotheses)}\nHard cutover; CHANGELOG BREAKING; no flags/shims/licensed rows.`, base: measure.sha, run_key: `${RUN_ID}-improve` }) }
catch (error) { sprint = { passed: false, error: String(error) } }
const sprintValid = sprint && sprint.passed && /^integration\//.test(sprint.integration_branch || '') && /^[0-9a-f]{40}$/i.test(sprint.integration_sha || '') && validSuccessEvidence(sprint.gate_evidence)
if (!sprintValid) return { iteration: measure.iter, capability_state: 'ready', verdict: 'FAILED', holdout: null, confirmed: [], refuted: [], sprint: sprint || null, ledger: [], ratchet_evidence: [], failure: 'Sprint incomplete/invalid; no holdout and no REJECT increment', run_id: RUN_ID, base_sha: BASE_SHA, canonical_after: null }

phase('Ratchet Prepare')
const ratchetBranch = `lane/oracle-ratchet-${RUN_SLUG}`
const ratchetHeartbeat = `${RUN_SLUG}-ratchet`
const ratchet = await agent(`PREPARE ONLY; never update canonical or choose authoritative verdict. Lease keeper-backed ${ratchetBranch} at exact ${sprint.integration_branch}@${sprint.integration_sha}, RunId=${RUN_ID}, HeartbeatId=${ratchetHeartbeat}. Recompute digest=${capability.holdout_digest_receipt}. Run these exact gate commands: ${JSON.stringify(RATCHET_GATE_COMMANDS)}. Return every metric in the workflow-frozen contract ${JSON.stringify(expectedRatchetMetrics(attributionPayload))}; candidate values must be copied from the exact typed gate JSON and deltas computed from them. For suspect exclusions use only frozen candidates ${JSON.stringify(attributionPayload.oracle_suspect_cells)} and typed NBBO means/distances from exact market-check commands. Prepare/commit scorecard and memory with memory_verdict derived from rules, release. Workflow independently validates and computes verdict/CAS.`, { agentType: 'vol-verifier', schema: RATCHET_PREPARE, label: 'ratchet-prepare' })
const ratchetError = ratchetPrepareContractError(ratchet, { tested_sha: sprint.integration_sha, tested_branch: sprint.integration_branch, ratchet_branch: ratchetBranch, holdout_digest: capability.holdout_digest_receipt, run_id: RUN_ID, heartbeat_id: ratchetHeartbeat, applicable_modes: applicableModes, baseline_contract: attributionPayload, suspect_candidates: attributionPayload.oracle_suspect_cells })
if (ratchetError) return { iteration: measure.iter, capability_state: 'ready', verdict: 'FAILED', holdout: null, confirmed: [], refuted: [], sprint: { passed: true, integration_branch: sprint.integration_branch, integration_sha: sprint.integration_sha }, ledger: [], ratchet_evidence: [], ratchet: null, failure: ratchetError, run_id: RUN_ID, base_sha: BASE_SHA, canonical_after: null }
const computedVerdict = computeRatchetVerdict(ratchet)
if (ratchet.memory_verdict !== computedVerdict) return { iteration: measure.iter, capability_state: 'ready', verdict: 'FAILED', holdout: null, confirmed: [], refuted: [], sprint: { passed: true, integration_branch: sprint.integration_branch, integration_sha: sprint.integration_sha }, ledger: [], ratchet_evidence: [], ratchet: null, failure: 'prepared memory verdict disagrees with workflow computation', run_id: RUN_ID, base_sha: BASE_SHA, canonical_after: null }

let finalize = null
let finalizeError = null
let finalizeThrown = null
if (computedVerdict === 'ACCEPT') {
  phase('Ratchet Finalize')
  const expected = { ref: CANONICAL_REF, new_sha: ratchet.ratchet_sha, expected_old_sha: BASE_SHA }
  try {
    finalize = await agent(`Run exactly git update-ref ${CANONICAL_REF} ${ratchet.ratchet_sha} ${BASE_SHA}; do nothing else; return typed receipt.`, { agentType: 'vol-ref-finalizer', schema: CAS_RECEIPT, label: 'ratchet-cas-finalizer' })
  } catch (error) { finalizeThrown = String(error) }
  finalizeError = casReceiptError(finalize, expected)
}
phase('Ratchet Audit')
let audit = null
let auditThrown = null
try {
  audit = await agent(`Read-only: run exactly git rev-parse ${CANONICAL_REF}; return actual full SHA.`, { agentType: 'vol-ref-auditor', schema: HEAD_RECEIPT, label: 'ratchet-post-decision-audit' })
} catch (error) { auditThrown = String(error) }
const auditError = auditThrown || auditReceiptError(audit, CANONICAL_REF)
const canonicalAfter = auditError || audit.sha === 'MISSING' ? null : audit.sha
const transactionOk = computedVerdict === 'ACCEPT' ? !finalizeError && !auditError && canonicalAfter === ratchet.ratchet_sha : !auditError && canonicalAfter === BASE_SHA
const verdict = transactionOk ? computedVerdict : 'FAILED'
const transactionEvidence = [
  ...ratchet.evidence,
  { command: ratchet.acquisition_receipt.action, exit_code: ratchet.acquisition_receipt.exit_code, output: ratchet.acquisition_receipt.output },
  { command: ratchet.digest_receipt.command, exit_code: ratchet.digest_receipt.exit_code, output: ratchet.digest_receipt.output },
  ...ratchet.metric_evidence,
  ...ratchet.gate_receipts.map(receipt => ({ command: receipt.command, exit_code: receipt.exit_code, output: receipt.output })),
  ...ratchet.market_evidence.map(receipt => ({ command: receipt.command, exit_code: receipt.exit_code, output: receipt.output })),
  { command: ratchet.release_receipt.action, exit_code: ratchet.release_receipt.exit_code, output: ratchet.release_receipt.output },
  ...(finalize ? [{ command: finalize.command, exit_code: finalize.exit_code, output: finalize.output }] : []),
  ...(audit ? [{ command: audit.command, exit_code: audit.exit_code, output: audit.output }] : []),
]
return {
  iteration: measure.iter, capability_state: 'ready', verdict,
  holdout: verdict === 'FAILED' ? null : { summary: ratchet.holdout_summary, metrics: ratchet.metrics, oracle_suspects_excluded: ratchet.oracle_suspects_excluded },
  confirmed: verdict === 'FAILED' ? [] : ratchet.hypotheses_confirmed, refuted: verdict === 'FAILED' ? [] : ratchet.hypotheses_refuted,
  sprint: { passed: true, integration_branch: sprint.integration_branch, integration_sha: sprint.integration_sha }, ledger: verdict === 'FAILED' ? [] : ratchet.ledger_appended,
  ratchet_evidence: transactionEvidence, ratchet: { prepare: ratchet, computed_verdict: computedVerdict, finalize, audit },
  failure: transactionOk ? null : (finalizeThrown || finalizeError || auditError || 'canonical post-decision mismatch'),
  landing_status: transactionOk ? (computedVerdict === 'ACCEPT' ? 'COMMITTED' : 'UNCHANGED_REJECT') : (canonicalAfter === ratchet.ratchet_sha ? 'LANDED_AUDITED_WITH_INVALID_RECEIPT' : 'CANONICAL_MISMATCH'),
  run_id: RUN_ID, base_sha: BASE_SHA, canonical_after: canonicalAfter,
}
*/
