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
  { metric_id: 'mode_b_price_mae', mode: 'B', unit: 'ticks', limit: 2 },
  { metric_id: 'mode_b_vol_mae', mode: 'B', unit: 'bp', limit: 10 },
  { metric_id: 'mode_b_delta_rel', mode: 'B', unit: 'relative', limit: 0.02 },
  { metric_id: 'mode_b_gamma_rel', mode: 'B', unit: 'relative', limit: 0.02 },
  { metric_id: 'mode_b_theta_rel', mode: 'B', unit: 'relative', limit: 0.02 },
  { metric_id: 'mode_b_vega_rel', mode: 'B', unit: 'relative', limit: 0.02 },
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
  mode_b_targeted_tests: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate mode_b_targeted_tests',
  mode_b_smoke_tune: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate mode_b_smoke_tune',
})
const TARGETED_BOOTSTRAP_GATE_IDS = Object.freeze(['mode_a_targeted_tests', 'mode_a_smoke', 'convention_tests', 'mode_a_smoke_tune', 'residual_floor', 'mode_b_targeted_tests', 'mode_b_smoke_tune'])
const READY_MEASURE_GATES = Object.freeze({
  measure_mode_a: 'atx-vol-oracle-bench --cohort smoke,tune --mode A --scorecard --aggregate-only',
  measure_mode_b: 'atx-vol-oracle-bench --cohort smoke,tune --mode B --scorecard --aggregate-only',
  measure_speed: 'atx-vol-oracle-bench --cohort tune --benchmark-speed --preset rel-avx2 --quiet-host --aggregate-only',
})
const READY_MEASURE_COMMANDS = Object.freeze(Object.values(READY_MEASURE_GATES))
const ADOPTION_COMMAND = 'powershell scripts\\oracle-adopt-existing-data.ps1'
const MODE_A_RECEIPT_ONLY_PATHS = Object.freeze(['atx-vol/bench/oracle/bootstrap/mode-a.json'])

const EVIDENCE_ITEM = {
  type: 'object', additionalProperties: false, required: ['command', 'exit_code', 'output'],
  properties: { command: { type: 'string' }, exit_code: { type: 'integer' }, output: { type: 'string' } },
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
    action: { type: 'string', enum: ['acquire', 'release'] }, lease_name: { type: 'string' }, run_id: { type: 'string' },
    branch: { type: 'string' }, base_sha: { type: 'string' }, worktree: { type: 'string' }, heartbeat_id: { type: 'string' },
    keeper_pid: { type: 'integer' }, keeper_process_started_utc: { type: 'string' }, keeper_ready_utc: { type: 'string' }, exit_code: { type: 'integer' }, output: { type: 'string' },
  },
}
const HEAD_RECEIPT = {
  type: 'object', additionalProperties: false, required: ['ref', 'sha', 'command', 'exit_code', 'output'],
  properties: { ref: { type: 'string' }, sha: { type: 'string' }, command: { type: 'string' }, exit_code: { type: 'integer' }, output: { type: 'string' } },
}
const CAS_RECEIPT = {
  type: 'object', additionalProperties: false, required: ['ref', 'new_sha', 'expected_old_sha', 'command', 'exit_code', 'output'],
  properties: {
    ref: { type: 'string' }, new_sha: { type: 'string' }, expected_old_sha: { type: 'string' }, command: { type: 'string' },
    exit_code: { type: 'integer' }, output: { type: 'string' },
  },
}
const NUMERIC_GATE_METRIC = {
  type: 'object', additionalProperties: false, required: ['metric_id', 'value', 'count', 'unit'],
  properties: {
    metric_id: { type: 'string' }, value: { type: 'number' }, count: { type: 'integer' }, unit: { type: 'string' }, pin: { type: 'number' },
  },
}
const GATE_RECEIPT = {
  type: 'object', additionalProperties: false, required: ['gate_id', 'command', 'exit_code', 'output', 'result'],
  properties: {
    gate_id: { type: 'string', enum: Object.keys(BOOTSTRAP_GATE_COMMANDS) }, command: { type: 'string' }, exit_code: { type: 'integer' }, output: { type: 'string' },
    result: {
      type: 'object', additionalProperties: false, required: ['schema_version', 'status', 'observations', 'command_id', 'raw_output_sha256'],
      properties: {
        schema_version: { type: 'integer', enum: [1] }, status: { type: 'string', enum: ['PASS'] }, observations: { type: 'integer' },
        command_id: { type: 'string', enum: Object.keys(BOOTSTRAP_GATE_COMMANDS) }, raw_output_sha256: { type: 'string', pattern: '^[0-9a-f]{64}$' },
        gate_kind: { type: 'string', enum: ['ctest', 'oracle_bench'] }, tests_executed: { type: 'integer' }, tests_passed: { type: 'integer' },
        rows_processed: { type: 'integer' }, metric_ids: { type: 'array', items: { type: 'string' } }, audit_summary: { type: 'string' },
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
  type: 'object', additionalProperties: false, required: ['gate_id', 'command', 'status', 'exit_code', 'output'],
  properties: {
    gate_id: { type: 'string', enum: ['mode_a_targeted_tests', 'mode_a_smoke'] }, command: { type: 'string' },
    status: { type: 'string', enum: ['PASS', 'FAIL'] }, exit_code: { type: 'integer' }, output: { type: 'string' },
  },
}
const CHANGED_PATH_RECEIPT = {
  type: 'object', additionalProperties: false, required: ['base_sha', 'tested_sha', 'command', 'exit_code', 'output', 'paths'],
  properties: {
    base_sha: { type: 'string' }, tested_sha: { type: 'string' }, command: { type: 'string' }, exit_code: { type: 'integer' },
    output: { type: 'string' }, paths: { type: 'array', items: { type: 'string' } },
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
  type: 'object', additionalProperties: false, required: ['reviewed_sha', 'head_after', 'command', 'exit_code', 'output'],
  properties: {
    reviewed_sha: { type: 'string' }, head_after: { type: 'string' }, command: { type: 'string' },
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
  required: ['state', 'canonical_ref', 'canonical_exists', 'base_ref', 'base_sha', 'holdout_digest_receipt', 'next_iter', 'evidence'],
  properties: {
    state: { type: 'string', enum: ['missing_data', 'missing_mode_a', 'missing_conventions', 'missing_mode_b', 'ready'] },
    canonical_ref: { type: 'string' }, canonical_exists: { type: 'boolean' }, base_ref: { type: 'string' }, base_sha: { type: 'string' },
    holdout_digest_receipt: { type: 'string' }, next_iter: { type: 'string' }, evidence: { type: 'array', items: EVIDENCE_ITEM },
    diagnostics: { type: 'array', items: EVIDENCE_ITEM },
  },
}
const BOOTSTRAP_REPORT_IDENTITY_REQUIRED = ['state', 'outcome', 'branch', 'sha', 'base_sha', 'worktree', 'lease_name', 'lease_run_id', 'heartbeat_id', 'keeper_pid', 'keeper_process_started_utc', 'acquisition_receipt', 'holdout_digest_receipt', 'evidence', 'deviations']
const BOOTSTRAP_REPORT_COMMON_PROPERTIES = {
  state: { type: 'string', enum: ['missing_data', 'missing_mode_a', 'missing_conventions', 'missing_mode_b'] },
  branch: { type: 'string', minLength: 1 }, sha: { type: 'string' }, base_sha: { type: 'string', pattern: '^[0-9a-f]{40}$' },
  worktree: { type: 'string', minLength: 1 }, lease_name: { type: 'string', pattern: '^pool-[0-9]+$' },
  lease_run_id: { type: 'string', minLength: 1 }, heartbeat_id: { type: 'string', minLength: 1 },
  keeper_pid: { type: 'integer' }, keeper_process_started_utc: { type: 'string', minLength: 1 }, acquisition_receipt: LEASE_RECEIPT,
  bootstrap_path: { type: 'string', enum: ['data_adoption', 'data_ingest', 'mode_a_receipt_only', 'mode_a_implementation', 'standard'] },
  adoption_receipt: ADOPTION_RECEIPT, disk_receipt: GATE_RECEIPT,
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
        outcome: { type: 'string', enum: ['DONE'] }, sha: { type: 'string', pattern: '^[0-9a-f]{40}$' },
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
        outcome: { type: 'string', enum: ['BLOCKED'] }, sha: { type: 'string', pattern: '^(?:|[0-9a-f]{40})$' },
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
  type: 'object', additionalProperties: false, required: ['verdict', 'reviewed_sha', 'evidence', 'findings'],
  properties: {
    verdict: { type: 'string', enum: ['APPROVE', 'BLOCK'] }, reviewed_sha: { type: 'string' },
    evidence: { type: 'array', items: EVIDENCE_ITEM }, diagnostics: { type: 'array', items: EVIDENCE_ITEM },
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
const CONVENTION_MAP = {
  type: 'object', additionalProperties: false,
  required: ['theta_basis', 'vega_basis', 'rate_model', 'dividend_model', 'day_count', 'sign_model'],
  properties: {
    theta_basis: { type: 'string', enum: ['per_day', 'per_year'] },
    vega_basis: { type: 'string', enum: ['per_vol_point', 'per_unit_vol'] },
    rate_model: { type: 'string', enum: ['continuous', 'simple'] },
    dividend_model: { type: 'string', enum: ['continuous_yield', 'discrete_cash'] },
    day_count: { type: 'string', enum: ['ACT_365F', 'ACT_360', 'BUS_252'] },
    sign_model: { type: 'string', enum: ['spiderrock'] },
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
  missing_data: { stage: '1', slug: 'data', next: 'missing_mode_a', gate_ids: ['aggregate_store', 'ingest_manifest', 'cohort_manifests', 'holdout_digest'], contract: 'First run exactly powershell scripts\\oracle-adopt-existing-data.ps1. ADOPTED means commit only the generated holdout.sha256 and exact v1 bootstrap/data.json receipt; do not ingest or require transient disk. INGEST_REQUIRED means verify >=15 GiB and licensed ZIP, then run the normal ingest/cohort path and produce the exact v1 schema with licensed-ingest command provenance. Never benchmark holdout or emit membership/rows.' },
  missing_mode_a: { stage: '2', slug: 'mode-a', next: 'missing_conventions', gate_ids: ['mode_a_targeted_tests', 'mode_a_smoke'], contract: 'Run the exact targeted Mode A gates first. If the already-present implementation passes, make no pricing implementation change and write only bootstrap/mode-a.json. Implement/fix Mode A only when an exact targeted gate proves it necessary. Do not implement/stub Mode B; never benchmark holdout.' },
  missing_conventions: { stage: '3', slug: 'conventions', next: 'missing_mode_b', gate_ids: ['convention_tests', 'mode_a_smoke_tune', 'residual_floor'], contract: 'Resolve conventions on aggregate smoke+tune Mode A, commit CONVENTIONS.md + iter-000 + exact v1 bootstrap/conventions.json validation/provenance receipt and evidenced memory. Never benchmark holdout or read Mode B.' },
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
    (action === 'release' || (receipt.output.includes(String(receipt.keeper_pid)) && receipt.output.includes(receipt.heartbeat_id) && receipt.output.includes(receipt.keeper_ready_utc)))
}

function validHeadReceipt(receipt, ref, sha) {
  return !!receipt && receipt.ref === ref && receipt.sha === sha && receipt.exit_code === 0 && receipt.command.trim() === `git rev-parse ${ref}` && receipt.output.trim() === sha
}

function validGateReceipt(receipt, gateId) {
  if (!receipt || receipt.gate_id !== gateId || receipt.command !== BOOTSTRAP_GATE_COMMANDS[gateId] || receipt.exit_code !== 0) return false
  const result = receipt.result
  if (!result || result.schema_version !== 1 || result.status !== 'PASS' || result.command_id !== gateId ||
      !Number.isInteger(result.observations) || result.observations <= 0 || !/^[0-9a-f]{64}$/.test(result.raw_output_sha256 || '') ||
      receipt.output !== JSON.stringify(result)) return false
  const commonKeys = ['schema_version', 'status', 'observations', 'command_id', 'raw_output_sha256']
  if (!TARGETED_BOOTSTRAP_GATE_IDS.includes(gateId)) return Object.keys(result).sort().join(',') === commonKeys.sort().join(',')
  const semanticKeys = [...commonKeys, 'gate_kind', 'tests_executed', 'tests_passed', 'rows_processed', 'metric_ids', 'audit_summary']
  if (Object.keys(result).sort().join(',') !== semanticKeys.sort().join(',')) return false
  if (gateId.endsWith('_tests')) return result.gate_kind === 'ctest' && Number.isInteger(result.tests_executed) && result.tests_executed > 0 &&
    result.tests_passed === result.tests_executed && result.rows_processed === 0 && Array.isArray(result.metric_ids) && result.metric_ids.length === 0 &&
    result.audit_summary === `tests_executed=${result.tests_executed} tests_passed=${result.tests_passed}`
  const wanted = expectedBootstrapMetricIds(gateId)
  return result.gate_kind === 'oracle_bench' && result.tests_executed === 0 && result.tests_passed === 0 && Number.isInteger(result.rows_processed) && result.rows_processed > 0 &&
    Array.isArray(result.metric_ids) && result.metric_ids.length === wanted.length && new Set(result.metric_ids).size === wanted.length && wanted.every(id => result.metric_ids.includes(id)) &&
    result.audit_summary === `status=PASS rows_processed=${result.rows_processed} metric_ids=${[...result.metric_ids].sort().join(',')}`
}

function expectedBootstrapMetricIds(gateId) {
  if (['mode_a_smoke', 'mode_a_smoke_tune', 'residual_floor'].includes(gateId)) return TARGET_REGISTRY.filter(item => item.mode === 'A').map(item => item.metric_id)
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
  return receipt.output === JSON.stringify(result)
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
    receipt.output === JSON.stringify(result)
}

function validIntegrationCommand(receipt, reviewedSha) {
  return !!receipt && receipt.reviewed_sha === reviewedSha && receipt.exit_code === 0 &&
    new RegExp(`^git\\s+(?:merge(?:\\s+--(?:ff-only|no-ff|no-edit))*|cherry-pick(?:\\s+--(?:ff|no-commit))*)\\s+${reviewedSha}$`, 'i').test(String(receipt.command || '').trim())
}

function casReceiptError(receipt, expected) {
  if (!receipt) return 'CAS receipt missing'
  if (receipt.ref !== expected.ref || receipt.new_sha !== expected.new_sha || receipt.expected_old_sha !== expected.expected_old_sha) return 'CAS identity mismatch'
  if (receipt.command.trim() !== `git update-ref ${expected.ref} ${expected.new_sha} ${expected.expected_old_sha}` || receipt.exit_code !== 0 || !String(receipt.output || '').trim()) return 'CAS command receipt invalid'
  return null
}

function auditReceiptError(receipt, ref) {
  if (!receipt) return 'canonical audit missing'
  if (receipt.ref !== ref || receipt.command.trim() !== `git rev-parse ${ref}` ||
      (!/^[0-9a-f]{40}$/i.test(receipt.sha || '') && receipt.sha !== 'MISSING') || receipt.output.trim() !== receipt.sha) return 'canonical audit invalid'
  if ((receipt.sha === 'MISSING' && receipt.exit_code === 0) || (receipt.sha !== 'MISSING' && receipt.exit_code !== 0)) return 'canonical audit invalid'
  return null
}

function reviewContractError(review, expectedSha) {
  if (!review) return 'missing review'
  if (!['APPROVE', 'BLOCK'].includes(review.verdict) || !Array.isArray(review.findings)) return 'review shape invalid'
  if (review.reviewed_sha !== expectedSha) return 'reviewed SHA mismatch'
  if (!validSuccessEvidence(review.evidence) || diagnosticsUseForbiddenCommand(review.diagnostics)) return 'review has invalid/broad command evidence'
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

function validPrecheckGateReceipt(receipt, gateId) {
  if (!receipt || receipt.gate_id !== gateId || receipt.command !== BOOTSTRAP_GATE_COMMANDS[gateId] || !String(receipt.output || '').trim()) return false
  if (receipt.status === 'FAIL') return Number.isInteger(receipt.exit_code) && receipt.exit_code !== 0
  if (receipt.status !== 'PASS' || receipt.exit_code !== 0) return false
  let result
  try { result = JSON.parse(receipt.output) } catch { return false }
  return validGateReceipt({ gate_id: gateId, command: receipt.command, exit_code: receipt.exit_code, output: receipt.output, result }, gateId)
}

function bootstrapPathError(report, expected) {
  if (!['missing_data', 'missing_mode_a'].includes(expected.state)) return null
  if (!validChangedPathReceipt(report.changed_path_receipt, report)) return 'bootstrap changed-path receipt invalid'
  const paths = report.changed_path_receipt.paths
  if (expected.state === 'missing_data') {
    if (!validAdoptionReceipt(report.adoption_receipt, expected.base_sha)) return 'Stage1 typed adoption receipt invalid'
    const adoptionEvidence = report.evidence.filter(item => item.command === ADOPTION_COMMAND && item.exit_code === report.adoption_receipt.exit_code && item.output === report.adoption_receipt.output)
    if (adoptionEvidence.length !== 1) return 'Stage1 adoption evidence missing/untyped'
    const ingestEvidence = report.evidence.filter(item => /^python atx-vol\/scripts\/oracle_ingest\.py --zip \S+/.test(item.command || ''))
    const diskEvidence = report.evidence.filter(item => item.command === BOOTSTRAP_GATE_COMMANDS.disk)
    if (report.adoption_receipt.result.status === 'ADOPTED') {
      if (report.bootstrap_path !== 'data_adoption' || Object.prototype.hasOwnProperty.call(report, 'disk_receipt') || ingestEvidence.length || diskEvidence.length) return 'Stage1 ADOPTED path contradicted by disk/ingest'
      if (report.adoption_receipt.result.holdout_membership_sha256 !== report.holdout_digest_receipt) return 'Stage1 ADOPTED digest receipt mismatch'
      const wanted = ['atx-vol/bench/oracle/bootstrap/data.json', 'atx-vol/bench/oracle/cohorts/holdout.sha256']
      return paths.length === wanted.length && wanted.every((path, index) => paths[index] === path) ? null : 'Stage1 ADOPTED changed paths invalid'
    }
    if (report.bootstrap_path !== 'data_ingest' || !validGateReceipt(report.disk_receipt, 'disk') || report.disk_receipt.result.observations < 15) return 'Stage1 INGEST_REQUIRED disk receipt invalid'
    if (diskEvidence.length !== 1 || diskEvidence[0].output !== report.disk_receipt.output || ingestEvidence.length !== 1 || ingestEvidence[0].exit_code !== 0 || !String(ingestEvidence[0].output || '').trim()) return 'Stage1 INGEST_REQUIRED evidence missing/untyped'
    for (const path of ['atx-vol/bench/oracle/bootstrap/data.json', 'atx-vol/bench/oracle/cohorts/holdout.sha256']) if (!paths.includes(path)) return 'Stage1 ingest receipt paths missing'
    return null
  }
  if (Object.prototype.hasOwnProperty.call(report, 'adoption_receipt') || Object.prototype.hasOwnProperty.call(report, 'disk_receipt')) return 'Stage2 contains Stage1 receipt'
  if (!Array.isArray(report.precheck_gate_receipts) || report.precheck_gate_receipts.length !== 2) return 'Stage2 precheck receipt set invalid'
  const gateIds = ['mode_a_targeted_tests', 'mode_a_smoke']
  for (const gateId of gateIds) {
    const matches = report.precheck_gate_receipts.filter(receipt => receipt && receipt.gate_id === gateId)
    if (matches.length !== 1 || !validPrecheckGateReceipt(matches[0], gateId)) return `Stage2 precheck receipt invalid: ${gateId}`
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
  if (typeof report.deviations !== 'string') return 'bootstrap deviations invalid'
  if (report.outcome === 'BLOCKED') {
    if ((report.sha && !/^[0-9a-f]{40}$/.test(report.sha)) ||
        (report.holdout_digest_receipt && !/^[0-9a-f]{64}$/.test(report.holdout_digest_receipt))) return 'blocked bootstrap SHA/receipt invalid'
    if (!Array.isArray(report.blockers) || !report.blockers.length || report.blockers.some(item => typeof item !== 'string' || !item.trim())) return 'blocked bootstrap blockers missing'
    if (!validBootstrapSuccessEvidence(report.evidence, false)) return 'blocked bootstrap success evidence invalid'
    if (!validBootstrapDiagnostics(report.diagnostics, true)) return 'blocked bootstrap diagnostics invalid'
    return `bootstrap blocked: ${report.blockers.join('; ')}`
  }
  if (report.outcome !== 'DONE') return 'bootstrap build incomplete'
  if (!/^[0-9a-f]{40}$/.test(report.sha || '') || !/^[0-9a-f]{64}$/.test(report.holdout_digest_receipt || '') ||
      (expected.holdout_digest_receipt && report.holdout_digest_receipt !== expected.holdout_digest_receipt) || !validBootstrapSuccessEvidence(report.evidence, true) ||
      !validBootstrapDiagnostics(report.diagnostics, false)) return 'bootstrap build evidence/SHA/receipt invalid'
  if (report.bootstrap_path !== undefined && !['data_adoption', 'data_ingest', 'mode_a_receipt_only', 'mode_a_implementation', 'standard'].includes(report.bootstrap_path)) return 'bootstrap path invalid'
  const pathError = bootstrapPathError(report, expected)
  if (pathError) return pathError
  return null
}

function bootstrapPrepareError(report, review, prepare, expected) {
  const buildError = bootstrapReportError(report, expected)
  if (buildError) return buildError
  const reviewError = reviewContractError(review, report.sha)
  if (reviewError || review.verdict !== 'APPROVE') return reviewError || 'bootstrap not approved'
  if (!prepare || !prepare.passed || !validSuccessEvidence(prepare.evidence) || diagnosticsUseForbiddenCommand(prepare.diagnostics)) return 'scoped verifier missing/failed'
  if (prepare.reviewed_sha !== report.sha || prepare.integration_sha !== report.sha || prepare.integration_branch !== expected.integration_branch ||
      prepare.lease_run_id !== expected.run_id || prepare.next_state !== expected.next_state || prepare.holdout_digest_receipt !== report.holdout_digest_receipt) return 'bootstrap prepare identity/state mismatch'
  if (!/^pool-[0-9]+$/.test(prepare.integration_lease || '') || !/[\\/]atx-wt[\\/]pool-[0-9]+$/i.test(prepare.integration_worktree || '') ||
      !prepare.integration_worktree.replace(/\//g, '\\').toLowerCase().endsWith(`\\${prepare.integration_lease.toLowerCase()}`)) return 'bootstrap verifier not isolated'
  const laneLease = { lease_name: report.lease_name, run_id: expected.run_id, branch: expected.branch, base_sha: expected.base_sha, worktree: report.worktree, heartbeat_id: expected.heartbeat_id, keeper_pid: report.keeper_pid, keeper_process_started_utc: report.keeper_process_started_utc }
  const integrationLease = { lease_name: prepare.integration_lease, run_id: expected.run_id, branch: expected.integration_branch, base_sha: expected.base_sha, worktree: prepare.integration_worktree, heartbeat_id: expected.integration_heartbeat_id, keeper_pid: prepare.keeper_pid, keeper_process_started_utc: prepare.keeper_process_started_utc }
  if (!validLeaseReceipt(prepare.lane_release_receipt, laneLease, 'release')) return 'bootstrap lane release receipt invalid'
  if (!validLeaseReceipt(prepare.acquisition_receipt, integrationLease, 'acquire')) return 'bootstrap integration acquisition receipt invalid'
  if (!validLeaseReceipt(prepare.integration_release_receipt, integrationLease, 'release')) return 'bootstrap integration release receipt invalid'
  if (!validIntegrationCommand(prepare.integration_receipt, report.sha) || prepare.integration_receipt.head_after !== report.sha ||
      !String(prepare.integration_receipt.output || '').includes(report.sha)) return 'exact reviewed SHA integration receipt invalid'
  if (!validHeadReceipt(prepare.head_receipt, 'HEAD', report.sha)) return 'bootstrap HEAD receipt invalid'
  if (!Array.isArray(prepare.gate_receipts) || prepare.gate_receipts.length !== expected.gate_ids.length) return 'bootstrap gate receipt set mismatch'
  for (const gateId of expected.gate_ids) {
    const matches = prepare.gate_receipts.filter(receipt => receipt && receipt.gate_id === gateId)
    if (matches.length !== 1 || !validGateReceipt(matches[0], gateId)) return `bootstrap required gate receipt invalid: ${gateId}`
  }
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
  const conventionKeys = ['theta_basis', 'vega_basis', 'rate_model', 'dividend_model', 'day_count', 'sign_model']
  const conventions = payload.conventions
  if (!conventions || typeof conventions !== 'object' || Array.isArray(conventions) || !exactKeys(conventions, conventionKeys) ||
      !['per_day', 'per_year'].includes(conventions.theta_basis) || !['per_vol_point', 'per_unit_vol'].includes(conventions.vega_basis) ||
      !['continuous', 'simple'].includes(conventions.rate_model) || !['continuous_yield', 'discrete_cash'].includes(conventions.dividend_model) ||
      !['ACT_365F', 'ACT_360', 'BUS_252'].includes(conventions.day_count) || conventions.sign_model !== 'spiderrock') return 'aggregate convention map invalid'
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
  `Run exactly powershell scripts\\oracle-capability.ps1. Return its JSON unchanged. No other command or file access.`,
  { agentType: 'vol-capability-inspector', schema: CAPABILITY, label: 'capability' },
)
if (!capability || capability.canonical_ref !== CANONICAL_REF || !/^[0-9a-f]{40}$/i.test(capability.base_sha || '') || !validSuccessEvidence(capability.evidence)) throw new Error('capability freeze invalid')
if (capability.base_ref !== (capability.canonical_exists ? CANONICAL_REF : REQUESTED_BASE)) throw new Error('capability base-ref selection invalid')
if (capability.evidence.length !== 1 || capability.evidence[0].command !== 'powershell scripts\\oracle-capability.ps1' ||
    !capability.evidence[0].output.includes(`state=${capability.state}`) ||
    !capability.evidence[0].output.includes(`canonical_exists=${String(capability.canonical_exists)}`)) throw new Error('capability probe receipt invalid')
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
  const expected = { state: capability.state, branch, base_sha: BASE_SHA, run_id: RUN_ID, heartbeat_id: heartbeat, holdout_digest_receipt: capability.holdout_digest_receipt, integration_branch: integrationBranch, integration_heartbeat_id: integrationHeartbeat, next_state: lane.next, gate_ids: lane.gate_ids }
  phase('Bootstrap Build')
  let envelope = await agent(
    `ONE fixed bootstrap lane; no planner/holdout. Stage=${lane.stage}, base=${BASE_SHA}. ${lane.contract} Acquire ${branch} with RunId=${RUN_ID}, HeartbeatId=${heartbeat}; the independent keeper owns liveness. Implement, scoped-test, commit, keep lease, and return exactly {report:<typed report>}. Every evidence[].command and diagnostics[].command is one exact executed command only: no chaining, cwd annotation, control characters, or prose. DONE requires a committed lowercase SHA, nonempty success evidence, and only the raw lowercase 64-hex holdout_digest_receipt. Stage 1 DONE must return bootstrap_path, exact adoption_receipt, conditional disk_receipt, and exact base...SHA changed_path_receipt. For ADOPTED, adoption_receipt.command and its one matching evidence[].command must both equal exactly ${ADOPTION_COMMAND}, and that evidence output must exactly equal adoption_receipt.output. If work is BLOCKED after acquiring the lease, do not invent a digest, commit, or success: return the exact lease identity/acquisition receipt, sha='' and holdout_digest_receipt='' when unavailable, evidence=[] when no command succeeded (otherwise only exact exit-zero commands), plus nonempty blockers and typed nonempty diagnostics; the workflow will abort and release. Stage 2 DONE must return bootstrap_path, both typed pre-edit gate receipts, and exact base...SHA changed_path_receipt; PASS/PASS mechanically permits only bootstrap/mode-a.json.`,
    { agentType: 'vol-builder', schema: BOOTSTRAP_REPORT_TOOL_SCHEMA, label: `bootstrap-build:${capability.state}` },
  )
  let unwrapped = unwrapBootstrapReport(envelope)
  let report = unwrapped.report
  let reportError = unwrapped.error || bootstrapReportError(report, expected)
  let cleanupReport = report && !bootstrapLeaseIdentityError(report, expected) ? report : null
  let review = null
  if (!reportError) {
    phase('Bootstrap Review')
    review = await agent(`Fresh exact-SHA review ${BASE_SHA}...${report.sha}. Verify stage ${lane.stage}, no holdout, tests/evidence.`, { agentType: 'vol-reviewer', schema: REVIEW, label: `bootstrap-review:${capability.state}` })
    reportError = reviewContractError(review, report.sha)
  }
  if (!reportError && review.verdict === 'BLOCK') {
    phase('Bootstrap Fix')
    envelope = await agent(`Fix exactly blockers ${JSON.stringify(review.findings.filter(finding => finding.severity === 'blocker'))} in ${report.worktree}; keep same keeper lease, rerun checks, commit, and return exactly {report:<new typed report>} with new lowercase SHA/receipts.`, { agentType: 'vol-builder', schema: BOOTSTRAP_REPORT_TOOL_SCHEMA, label: `bootstrap-fix:${capability.state}` })
    unwrapped = unwrapBootstrapReport(envelope)
    report = unwrapped.report
    if (report && !bootstrapLeaseIdentityError(report, expected)) cleanupReport = report
    reportError = unwrapped.error || bootstrapReportError(report, expected)
    if (!reportError) {
      phase('Bootstrap Re-review')
      review = await agent(`FRESH post-Fix review of exactly ${report.sha}; never reuse prior verdict.`, { agentType: 'vol-reviewer', schema: REVIEW, label: `bootstrap-rereview:${capability.state}` })
      reportError = reviewContractError(review, report.sha)
    }
  }
  if (reportError || !review || review.verdict !== 'APPROVE') {
    let cleanup = null
    if (cleanupReport) cleanup = await agent(`Abort without integration. Release only ${cleanupReport.lease_name} with RunId=${RUN_ID}; return typed release.`, { agentType: 'vol-verifier', schema: CLEANUP, label: 'bootstrap-abort-cleanup' })
    return { iteration: `bootstrap-${lane.stage}`, capability_state: capability.state, verdict: 'FAILED', holdout: null, confirmed: [], refuted: [], sprint: null, ledger: [], ratchet_evidence: [], failure: reportError || 'bootstrap not approved', cleanup, run_id: RUN_ID, base_sha: BASE_SHA, canonical_after: null }
  }
  phase('Bootstrap Verify')
  const prepare = await agent(
    `PREPARE ONLY; never update canonical. Release ${report.lease_name}; acquire new keeper lease ${integrationBranch} at ${BASE_SHA}, RunId=${RUN_ID}, HeartbeatId=${integrationHeartbeat}. Integrate exact ${report.sha}, prove HEAD, run exactly gates ${JSON.stringify(lane.gate_ids)}, prove next_state=${lane.next} without holdout membership, release. Return typed release/acquire/integration/HEAD/gate/release receipts.`,
    { agentType: 'vol-verifier', schema: BOOTSTRAP_PREPARE, label: `bootstrap-prepare:${capability.state}` },
  )
  const prepareError = bootstrapPrepareError(report, review, prepare, expected)
  if (prepareError) return { iteration: `bootstrap-${lane.stage}`, capability_state: capability.state, verdict: 'FAILED', holdout: null, confirmed: [], refuted: [], sprint: null, ledger: [], ratchet_evidence: [], failure: prepareError, bootstrap: { report, review, prepare }, run_id: RUN_ID, base_sha: BASE_SHA, canonical_after: null }
  phase('Bootstrap Finalize')
  const casExpected = { ref: CANONICAL_REF, new_sha: report.sha, expected_old_sha: CANONICAL_EXPECTED_OLD }
  let finalize = null
  let finalizeThrown = null
  try {
    finalize = await agent(`Run exactly git update-ref ${CANONICAL_REF} ${report.sha} ${CANONICAL_EXPECTED_OLD}; do nothing else; return typed receipt.`, { agentType: 'vol-ref-finalizer', schema: CAS_RECEIPT, label: 'bootstrap-cas-finalizer' })
  } catch (error) { finalizeThrown = String(error) }
  const finalizeError = casReceiptError(finalize, casExpected)
  phase('Bootstrap Audit')
  let audit = null
  let auditThrown = null
  try {
    audit = await agent(`Read-only: run exactly git rev-parse ${CANONICAL_REF}; return actual SHA with exit 0, or sha/output=MISSING with the command's nonzero exit.`, { agentType: 'vol-ref-auditor', schema: HEAD_RECEIPT, label: 'bootstrap-post-cas-audit' })
  } catch (error) { auditThrown = String(error) }
  const auditError = auditThrown || auditReceiptError(audit, CANONICAL_REF)
  const canonicalAfter = auditError || audit.sha === 'MISSING' ? null : audit.sha
  const landed = !finalizeError && !auditError && canonicalAfter === report.sha
  return { iteration: `bootstrap-${lane.stage}`, capability_state: capability.state, verdict: landed ? 'BOOTSTRAP' : 'FAILED', holdout: null, confirmed: [], refuted: [], sprint: null, ledger: [], ratchet_evidence: [], bootstrap: { report, review, prepare, finalize, audit }, failure: landed ? null : (finalizeThrown || finalizeError || auditError || 'post-CAS canonical mismatch'), landing_status: landed ? 'COMMITTED' : (canonicalAfter === report.sha ? 'LANDED_AUDITED_WITH_INVALID_RECEIPT' : 'NOT_COMMITTED'), run_id: RUN_ID, base_sha: BASE_SHA, canonical_after: canonicalAfter }
}

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
