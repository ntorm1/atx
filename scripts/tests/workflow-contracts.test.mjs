import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'

const read = relative => readFileSync(resolve(process.cwd(), relative), 'utf8')
const oracle = read('.claude/workflows/vol-oracle-iter.js')
const sprint = read('.claude/workflows/vol-sprint.js')
const analyst = read('.claude/agents/vol-analyst.md')
const leases = read('scripts/lease-worktree.ps1')
const keeper = read('scripts/lease-heartbeat-keeper.ps1')
const capabilityProbe = read('scripts/oracle-capability.ps1')
const targetedGate = read('scripts/oracle-targeted-gate.ps1')
const capabilityAgent = read('.claude/agents/vol-capability-inspector.md')
const americanTests = read('atx-vol/tests/american_test.cpp')
const adjustedGreeksTests = read('atx-vol/tests/adjusted_greeks_test.cpp')
const settings = JSON.parse(read('.claude/settings.json'))
const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor

const SHA = {
  base: 'b'.repeat(40), build: 'a'.repeat(40), fixed: 'f'.repeat(40),
  measure: 'd'.repeat(40), integration: 'i'.repeat(40).replaceAll('i', '1'), ratchet: 'c'.repeat(40),
}
const DIGEST = 'e'.repeat(64)
const REF = 'refs/heads/oracle/canonical'
const okEvidence = [{ command: 'verify target', exit_code: 0, output: 'PASS' }]
const TARGET_REGISTRY = [
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
]
const AGGREGATE_REGISTRY = [
  { metric_id: 'mode_a_aggregate_error', mode: 'A', unit: 'relative' },
  { metric_id: 'mode_b_aggregate_error', mode: 'B', unit: 'relative' },
]
const SPEED_METRIC_ID = 'rel_avx2_rows_per_second'
const RATCHET_GATE_COMMANDS = {
  holdout_mode_a: 'atx-vol-oracle-bench --cohort holdout --mode A --aggregate-only',
  holdout_mode_b: 'atx-vol-oracle-bench --cohort holdout --mode B --aggregate-only',
  rel_avx2_speed: 'atx-vol-oracle-bench --cohort tune --benchmark-speed --preset rel-avx2 --quiet-host --aggregate-only',
}
const BOOTSTRAP_GATE_COMMANDS = {
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
}
const FIXED_ORACLE_GATES = [
  { gate_id: 'scorecard:mode_a_smoke_tune', command: 'atx-vol-oracle-bench --cohort smoke,tune --mode A --scorecard --aggregate-only' },
  { gate_id: 'scorecard:mode_b_smoke_tune', command: 'atx-vol-oracle-bench --cohort smoke,tune --mode B --scorecard --aggregate-only' },
  { gate_id: 'speed:rel_avx2_quiet', command: 'atx-vol-oracle-bench --cohort tune --benchmark-speed --preset rel-avx2 --quiet-host --aggregate-only' },
]
const READY_MEASURE_GATES = {
  measure_mode_a: FIXED_ORACLE_GATES[0].command,
  measure_mode_b: FIXED_ORACLE_GATES[1].command,
  measure_speed: FIXED_ORACLE_GATES[2].command,
}
const READY_MEASURE_COMMANDS = Object.values(READY_MEASURE_GATES)
const PATH_GATE_REGISTRY = [
  { owner: 'american-engine', exact_paths: ['atx-vol/src/pricing/american.cpp', 'atx-vol/include/atx/vol/api/pricing/american.hpp'], path_patterns: [], unit_targets: ['atx-vol-tests'], suite_prefixes: ['American', 'AdjustedGreeks'], oracle_tests: ['american-rsi', 'american-greeks-rsi'], pch_off_targets: ['atx-vol-tests'], mandatory_unit_targets: ['atx-vol-tests'], mandatory_unit_regexes: ['^AmericanGreeks.Delta_MatchesFd_Put$'], mandatory_oracle_tests: ['american-rsi'], mandatory_pch_off_targets: ['atx-vol-tests'] },
]
const UNIT_TEST_GATE_REGISTRY = {
  '^AmericanGreeks.Delta_MatchesFd_Put$': { adapter_gate: 'sprint_american_greeks_delta_put', source: 'atx-vol/tests/american_test.cpp' },
  '^AdjustedGreeks.FlatSmileLeavesDeltaUnchanged$': { adapter_gate: 'sprint_adjusted_greeks_flat_smile', source: 'atx-vol/tests/adjusted_greeks_test.cpp' },
}
const TARGETED_BOOTSTRAP_GATE_IDS = ['mode_a_targeted_tests', 'mode_a_smoke', 'convention_tests', 'mode_a_smoke_tune', 'residual_floor', 'mode_b_targeted_tests', 'mode_b_smoke_tune']

function extractFunction(source, name) {
  const start = source.indexOf(`function ${name}(`)
  assert.ok(start >= 0, `missing function ${name}`)
  const brace = source.indexOf('{', start)
  let depth = 0
  for (let index = brace; index < source.length; index += 1) {
    if (source[index] === '{') depth += 1
    if (source[index] === '}') {
      depth -= 1
      if (depth === 0) return source.slice(start, index + 1)
    }
  }
  throw new Error(`unterminated function ${name}`)
}

function loadFunctions(source, names, prelude = '') {
  const declarations = names.map(name => extractFunction(source, name)).join('\n')
  return Function(`${prelude}\n${declarations}; return { ${names.join(', ')} }`)()
}

const oracleFns = loadFunctions(oracle, [
  'iterationCommandError', 'validSuccessEvidence', 'exactEvidenceSet', 'diagnosticsUseForbiddenCommand', 'bootstrapCommandError', 'validBootstrapSuccessEvidence', 'validBootstrapDiagnostics', 'validLeaseReceipt', 'validHeadReceipt', 'validGateReceipt', 'expectedBootstrapMetricIds', 'expectedGateMetricIds', 'validRatchetGateReceipt', 'validIntegrationCommand', 'casReceiptError', 'auditReceiptError',
  'reviewContractError', 'validAdoptionReceipt', 'validChangedPathReceipt', 'validPrecheckGateReceipt', 'bootstrapPathError', 'bootstrapLeaseIdentityError', 'unwrapBootstrapReport', 'bootstrapReportError', 'bootstrapPrepareError', 'aggregatePayloadError', 'expectedMeasureMetricIds', 'expectedMetricUnit', 'validNumericMetric', 'validMeasureGateReceipt', 'measurePayloadError', 'attributionPayloadFromMeasure',
  'metricDeltaConsistent', 'expectedRatchetMetrics', 'sameNumber', 'distanceToInterval', 'marketCommand', 'marketReceiptError', 'relativeRegression', 'ratchetGateIdForMetric', 'ratchetCandidateValue', 'ratchetMetricsFromReceipts', 'ratchetPrepareContractError', 'computeRatchetVerdict',
], `const RATCHET_GATE_IDS = ['holdout_mode_a', 'holdout_mode_b', 'rel_avx2_speed']; const TARGETED_BOOTSTRAP_GATE_IDS = ${JSON.stringify(TARGETED_BOOTSTRAP_GATE_IDS)}; const TARGET_REGISTRY = ${JSON.stringify(TARGET_REGISTRY)}; const AGGREGATE_REGISTRY = ${JSON.stringify(AGGREGATE_REGISTRY)}; const SPEED_METRIC_ID = '${SPEED_METRIC_ID}'; const RATCHET_GATE_COMMANDS = ${JSON.stringify(RATCHET_GATE_COMMANDS)}; const BOOTSTRAP_GATE_COMMANDS = ${JSON.stringify(BOOTSTRAP_GATE_COMMANDS)}; const READY_MEASURE_GATES = ${JSON.stringify(READY_MEASURE_GATES)}; const READY_MEASURE_COMMANDS = ${JSON.stringify(READY_MEASURE_COMMANDS)}; const ADOPTION_COMMAND = 'powershell scripts\\oracle-adopt-existing-data.ps1'; const MODE_A_RECEIPT_ONLY_PATHS = ['atx-vol/bench/oracle/bootstrap/mode-a.json']`)
const sprintFns = loadFunctions(sprint, [
  'deterministicToken', 'sprintRunId', 'validSuccessEvidence', 'validLeaseReceipt', 'validHeadReceipt', 'validIntegrationCommand', 'changedHeader', 'changedCode', 'safeTarget', 'safeUnitRegex', 'safeOracleTest',
  'pathGateOwner', 'regexSuiteName', 'closureError', 'closureEntries', 'derivedGateRegistry', 'sprintUnitEvidenceError', 'oracleLoopCommandError', 'laneHeartbeatId', 'reportContractError', 'reviewContractError', 'laneFailureReason', 'gateContractError',
], `const FIXED_ORACLE_GATES = ${JSON.stringify(FIXED_ORACLE_GATES)}; const UNIT_TEST_GATE_REGISTRY = ${JSON.stringify(UNIT_TEST_GATE_REGISTRY)}; const PATH_GATE_REGISTRY = ${JSON.stringify(PATH_GATE_REGISTRY)}; const RUN_ID = 'run-1'; const RUN_SLUG = 'run-1'`)

function leaseIdentity({ lease = 'pool-1', branch, base, heartbeat, worktree = `C:\\atx-wt\\${lease}`, keeperPid = 4242 }) {
  return {
    lease_name: lease, run_id: 'run-1', branch, base_sha: base, worktree,
    heartbeat_id: heartbeat, keeper_pid: keeperPid,
    keeper_process_started_utc: '2026-08-15T12:00:00.0000000Z',
    keeper_ready_utc: '2026-08-15T12:00:01.0000000Z',
  }
}

function leaseReceipt(action, identity) {
  return {
    action, ...identity, exit_code: 0,
    output: action === 'acquire'
      ? `LEASED ${identity.lease_name} ${identity.run_id} ${identity.heartbeat_id} keeper_pid=${identity.keeper_pid} keeper_ready_utc=${identity.keeper_ready_utc}`
      : `RELEASED ${identity.lease_name} ${identity.run_id}`,
  }
}

function gateReceipt(gateId) {
  return { gate_id: gateId, command: `powershell scripts\\atx-build.ps1 -Ctest -R ${gateId}`, exit_code: 0, output: `${gateId} PASS` }
}

function bootstrapGateReceipt(gateId) {
  const result = { schema_version: 1, status: 'PASS', observations: 100, command_id: gateId, raw_output_sha256: 'a'.repeat(64) }
  if (TARGETED_BOOTSTRAP_GATE_IDS.includes(gateId)) {
    const ctest = gateId.endsWith('_tests')
    const metricIds = gateId.startsWith('mode_b') ? TARGET_REGISTRY.filter(item => item.mode === 'B').map(item => item.metric_id) : TARGET_REGISTRY.filter(item => item.mode === 'A').map(item => item.metric_id)
    Object.assign(result, ctest
      ? { gate_kind: 'ctest', tests_executed: 2, tests_passed: 2, rows_processed: 0, metric_ids: [], audit_summary: 'tests_executed=2 tests_passed=2' }
      : { gate_kind: 'oracle_bench', tests_executed: 0, tests_passed: 0, rows_processed: 100, metric_ids: metricIds, audit_summary: `status=PASS rows_processed=100 metric_ids=${[...metricIds].sort().join(',')}` })
  }
  return { gate_id: gateId, command: BOOTSTRAP_GATE_COMMANDS[gateId], exit_code: 0, output: JSON.stringify(result), result }
}

function ratchetGateReceipt(gateId, metrics) {
  const metricIds = gateId === 'holdout_mode_a' ? [...TARGET_REGISTRY, ...AGGREGATE_REGISTRY].filter(item => item.mode === 'A').map(item => item.metric_id)
    : gateId === 'holdout_mode_b' ? [...TARGET_REGISTRY, ...AGGREGATE_REGISTRY].filter(item => item.mode === 'B').map(item => item.metric_id) : [SPEED_METRIC_ID]
  const result = { schema_version: 1, status: 'PASS', command_id: gateId, observations: 100, metrics: metricIds.map(metricId => {
    const candidate = metrics.find(metric => metric.metric_id === metricId).candidate
    return { metric_id: metricId, value: candidate, count: 100, unit: metricId === SPEED_METRIC_ID ? 'rows_per_second' : [...TARGET_REGISTRY, ...AGGREGATE_REGISTRY].find(item => item.metric_id === metricId).unit }
  }) }
  return { gate_id: gateId, command: RATCHET_GATE_COMMANDS[gateId], exit_code: 0, output: JSON.stringify(result), result }
}

function sprintGateOutput(gate) {
  if (!gate.gate_id.startsWith('unit-test:')) return `${gate.gate_id} PASS`
  const commandId = gate.command.match(/-Gate\s+(\S+)/)[1]
  return JSON.stringify({
    schema_version: 1, status: 'PASS', observations: 2, command_id: commandId, gate_kind: 'ctest',
    tests_executed: 2, tests_passed: 2, rows_processed: 0, metric_ids: [],
    audit_summary: 'tests_executed=2 tests_passed=2', raw_output_sha256: 'a'.repeat(64),
  })
}

function auditReceipt(sha, { ref = REF, exitCode = 0 } = {}) {
  return { ref, sha, command: `git rev-parse ${ref}`, exit_code: exitCode, output: sha }
}

function casReceipt(newSha, { ref = REF, oldSha = SHA.base } = {}) {
  return {
    ref, new_sha: newSha, expected_old_sha: oldSha,
    command: `git update-ref ${ref} ${newSha} ${oldSha}`,
    exit_code: 0, output: 'update-ref exit 0',
  }
}

function bootstrapReport(sha = SHA.build) {
  const branch = 'lane/oracle-bootstrap-mode-a-run-1'
  const identity = leaseIdentity({ branch, base: SHA.base, heartbeat: 'run-1-bootstrap-mode-a' })
  return {
    state: 'missing_mode_a', outcome: 'DONE', branch, sha, base_sha: SHA.base,
    worktree: identity.worktree, lease_name: identity.lease_name, lease_run_id: 'run-1',
    heartbeat_id: identity.heartbeat_id, keeper_pid: identity.keeper_pid,
    keeper_process_started_utc: identity.keeper_process_started_utc,
    acquisition_receipt: leaseReceipt('acquire', identity), holdout_digest_receipt: DIGEST,
    bootstrap_path: 'mode_a_receipt_only',
    precheck_gate_receipts: ['mode_a_targeted_tests', 'mode_a_smoke'].map(gateId => {
      const receipt = bootstrapGateReceipt(gateId)
      return { gate_id: gateId, command: receipt.command, status: 'PASS', exit_code: 0, output: receipt.output }
    }),
    changed_path_receipt: {
      base_sha: SHA.base, tested_sha: sha, command: `git diff --name-only ${SHA.base}...${sha}`, exit_code: 0,
      output: 'atx-vol/bench/oracle/bootstrap/mode-a.json', paths: ['atx-vol/bench/oracle/bootstrap/mode-a.json'],
    },
    evidence: okEvidence, deviations: '',
  }
}

function bootstrapPrepare(report) {
  const integrationIdentity = leaseIdentity({
    lease: 'pool-2', branch: 'integration/oracle-bootstrap-mode-a-run-1', base: SHA.base,
    heartbeat: 'run-1-bootstrap-integration', keeperPid: 4343,
  })
  const laneIdentity = leaseIdentity({
    lease: report.lease_name, branch: report.branch, base: SHA.base, heartbeat: report.heartbeat_id,
    worktree: report.worktree, keeperPid: report.keeper_pid,
  })
  return {
    passed: true, reviewed_sha: report.sha, integration_branch: integrationIdentity.branch,
    integration_sha: report.sha, integration_worktree: integrationIdentity.worktree,
    integration_lease: integrationIdentity.lease_name, lease_run_id: 'run-1',
    integration_heartbeat_id: integrationIdentity.heartbeat_id, keeper_pid: integrationIdentity.keeper_pid,
    keeper_process_started_utc: integrationIdentity.keeper_process_started_utc,
    holdout_digest_receipt: DIGEST, next_state: 'missing_conventions',
    lane_release_receipt: leaseReceipt('release', laneIdentity),
    acquisition_receipt: leaseReceipt('acquire', integrationIdentity),
    integration_receipt: {
      reviewed_sha: report.sha, head_after: report.sha, command: `git merge ${report.sha}`,
      exit_code: 0, output: report.sha,
    },
    head_receipt: auditReceipt(report.sha, { ref: 'HEAD' }),
    gate_receipts: [bootstrapGateReceipt('mode_a_targeted_tests'), bootstrapGateReceipt('mode_a_smoke')],
    integration_release_receipt: leaseReceipt('release', integrationIdentity), evidence: okEvidence,
  }
}

function blockedStage1Report() {
  const branch = 'lane/oracle-bootstrap-data-run-1'
  const identity = leaseIdentity({ branch, base: SHA.base, heartbeat: 'run-1-bootstrap-data' })
  return {
    state: 'missing_data', outcome: 'BLOCKED', branch, sha: '', base_sha: SHA.base,
    worktree: identity.worktree, lease_name: identity.lease_name, lease_run_id: 'run-1',
    heartbeat_id: identity.heartbeat_id, keeper_pid: identity.keeper_pid,
    keeper_process_started_utc: identity.keeper_process_started_utc,
    acquisition_receipt: leaseReceipt('acquire', identity), holdout_digest_receipt: '', evidence: [],
    blockers: ['licensed ZIP unavailable'],
    diagnostics: [{ command: 'powershell Test-Path licensed-oracle.zip', exit_code: 1, output: 'licensed-oracle.zip missing' }],
    deviations: 'licensed data unavailable',
  }
}

function capability(state = 'missing_mode_a') {
  const canonicalExists = true
  return {
    state, canonical_ref: REF, canonical_exists: canonicalExists, base_ref: REF, base_sha: SHA.base,
    holdout_digest_receipt: DIGEST, next_iter: 'iter-001',
    evidence: [{ command: 'powershell scripts\\oracle-capability.ps1', exit_code: 0, output: `state=${state} canonical_exists=${canonicalExists}` }],
  }
}

function safePayload() {
  return {
    schema_version: 2, iteration: 1,
    target_metrics: TARGET_REGISTRY.map((item, index) => ({ metric_id: item.metric_id, mode: item.mode, baseline: 100 + index, count: 1000, unit: item.unit })),
    aggregate_metrics: AGGREGATE_REGISTRY.map((item, index) => ({ metric_id: item.metric_id, mode: item.mode, baseline: 50 + index, count: 1000, unit: item.unit })),
    speed: { metric_id: SPEED_METRIC_ID, baseline: 110, pin: 100, unit: 'rows_per_second' },
    prior_refuted_ids: [0], oracle_suspect_cells: [7],
    conventions: { theta_basis: 'per_day', vega_basis: 'per_vol_point', rate_model: 'continuous', dividend_model: 'continuous_yield', day_count: 'ACT_365F', sign_model: 'spiderrock' },
  }
}

function measureGateReceipt(gateId, payload = safePayload()) {
  let source
  if (gateId === 'measure_mode_a') source = [...payload.target_metrics.filter(item => item.mode === 'A'), ...payload.aggregate_metrics.filter(item => item.mode === 'A')]
  else if (gateId === 'measure_mode_b') source = [...payload.target_metrics.filter(item => item.mode === 'B'), ...payload.aggregate_metrics.filter(item => item.mode === 'B')]
  else source = [payload.speed]
  const metrics = source.map(metric => ({
    metric_id: metric.metric_id, value: metric.baseline, count: metric.count || 1000, unit: metric.unit,
    ...(metric.metric_id === SPEED_METRIC_ID ? { pin: metric.pin } : {}),
  }))
  const result = { schema_version: 1, status: 'PASS', command_id: gateId, observations: 1000, metrics }
  return { gate_id: gateId, command: READY_MEASURE_GATES[gateId], exit_code: 0, output: JSON.stringify(result), result }
}

function measure() {
  const branch = 'lane/oracle-measure-iter-001-run-1'
  const identity = leaseIdentity({ branch, base: SHA.base, heartbeat: 'run-1-measure' })
  return {
    status: 'ok', iter: 'iter-001', scorecard_path: 'scorecard.json', branch, sha: SHA.measure, base_sha: SHA.base,
    worktree: identity.worktree, lease_name: identity.lease_name, lease_run_id: 'run-1', heartbeat_id: identity.heartbeat_id,
    keeper_pid: identity.keeper_pid, keeper_process_started_utc: identity.keeper_process_started_utc,
    acquisition_receipt: leaseReceipt('acquire', identity), release_receipt: leaseReceipt('release', identity),
    lease_released: true,
    analysis_context: { prior_refuted_ids: [0], oracle_suspect_cells: [7], conventions: safePayload().conventions },
    gate_receipts: Object.keys(READY_MEASURE_GATES).map(gateId => measureGateReceipt(gateId)),
    evidence: Object.keys(READY_MEASURE_GATES).map(gateId => { const receipt = measureGateReceipt(gateId); return { command: receipt.command, exit_code: receipt.exit_code, output: receipt.output } }),
  }
}

function ratchetPrepare({ reject = false } = {}) {
  const branch = 'lane/oracle-ratchet-run-1'
  const identity = leaseIdentity({ branch, base: SHA.integration, heartbeat: 'run-1-ratchet', lease: 'pool-3', keeperPid: 4444 })
  const baseline = safePayload()
  const expected = [
    ...TARGET_REGISTRY.map(item => ({ metric_id: item.metric_id, mode: item.mode, unit: item.unit, gate: 'target', direction: 'lower', baseline: baseline.target_metrics.find(metric => metric.metric_id === item.metric_id).baseline, pin: item.limit })),
    ...AGGREGATE_REGISTRY.map(item => ({ ...item, gate: 'aggregate', direction: 'lower', baseline: baseline.aggregate_metrics.find(metric => metric.metric_id === item.metric_id).baseline, pin: 0 })),
    { metric_id: SPEED_METRIC_ID, mode: 'ALL', gate: 'speed', direction: 'higher', baseline: baseline.speed.baseline, pin: baseline.speed.pin, unit: 'rows_per_second' },
  ]
  const metrics = expected.map((metric, index) => {
    const candidate = metric.gate === 'speed' ? 120 : metric.gate === 'aggregate' ? metric.baseline * 1.01 : metric.baseline - 1
    return { ...metric, candidate, delta: candidate - metric.baseline, evidence_index: index }
  })
  if (reject) {
    metrics[0].candidate = metrics[0].baseline + 1
    metrics[0].delta = 1
  }
  const metricEvidence = metrics.map(metric => ({
    command: metric.gate === 'speed' ? 'speed benchmark' : 'oracle holdout benchmark', exit_code: 0,
    output: `${metric.metric_id} ${metric.baseline} ${metric.candidate} ${metric.delta}`,
  }))
  return {
    tested_branch: 'integration/run-1', tested_sha: SHA.integration, base_sha: SHA.integration,
    ratchet_branch: branch, ratchet_sha: SHA.ratchet, worktree: identity.worktree, lease_name: identity.lease_name,
    lease_run_id: 'run-1', heartbeat_id: identity.heartbeat_id, keeper_pid: identity.keeper_pid,
    keeper_process_started_utc: identity.keeper_process_started_utc,
    acquisition_receipt: leaseReceipt('acquire', identity), release_receipt: leaseReceipt('release', identity),
    digest_receipt: { expected_digest: DIGEST, actual_digest: DIGEST, command: 'verify holdout digest', exit_code: 0, output: DIGEST },
    applicable_modes: ['A', 'B'], metrics, metric_evidence: metricEvidence,
    gate_receipts: ['holdout_mode_a', 'holdout_mode_b', 'rel_avx2_speed'].map(gateId => ratchetGateReceipt(gateId, metrics)),
    oracle_suspects_excluded: [7],
    market_evidence: [{
      cell_id: 7, nbbo_bid_mean: 99, nbbo_ask_mean: 101, atx_price_mean: 100, oracle_price_mean: 103,
      atx_nbbo_distance: 0, oracle_nbbo_distance: 2, sample_count: 100,
      command: 'atx-vol-oracle-bench --cohort holdout --market-check 7 --aggregate-only', exit_code: 0,
      output: JSON.stringify({ cell_id: 7, nbbo_bid_mean: 99, nbbo_ask_mean: 101, atx_price_mean: 100, oracle_price_mean: 103, atx_nbbo_distance: 0, oracle_nbbo_distance: 2, sample_count: 100 }),
    }],
    memory_verdict: reject ? 'REJECT' : 'ACCEPT', holdout_summary: 'aggregate holdout result',
    hypotheses_confirmed: reject ? [] : ['H-1'], hypotheses_refuted: reject ? ['H-1'] : [],
    ledger_appended: ['ratchet fact'], northstar_updated: true, evidence: okEvidence,
  }
}

async function runOracle(responses, sprintResult = null) {
  let source = oracle.replace('export const meta', 'const meta')
  source = source.replace('const RUN_ID = oracleRunId(BASE_SHA, capability.state, capability.next_iter)', "const RUN_ID = 'run-1'")
  const calls = []
  const phases = []
  const workflowCalls = []
  const queues = Object.fromEntries(Object.entries(responses).map(([key, value]) => [key, Array.isArray(value) ? [...value] : [value]]))
  const agent = async (_prompt, options) => {
    calls.push(options.label)
    const queue = queues[options.label]
    assert.ok(queue && queue.length, `unexpected agent label ${options.label}`)
    const value = queue.shift()
    return typeof value === 'function' ? value() : value
  }
  const workflow = async (name, workflowArgs) => {
    workflowCalls.push({ name, args: workflowArgs })
    return sprintResult
  }
  const phase = name => phases.push(name)
  const fn = new AsyncFunction('args', 'phase', 'agent', 'workflow', source)
  const result = await fn({}, phase, agent, workflow)
  return { result, phases, calls, workflowCalls }
}

function bootstrapResponses({ fix = false, prepareMutator, finalize = casReceipt(SHA.build), audit = auditReceipt(SHA.build) } = {}) {
  const first = bootstrapReport()
  const final = fix ? bootstrapReport(SHA.fixed) : first
  const prepare = bootstrapPrepare(final)
  if (prepareMutator) prepareMutator(prepare)
  const responses = {
    capability: capability(),
    'bootstrap-build:missing_mode_a': { report: first },
    'bootstrap-review:missing_mode_a': fix
      ? { verdict: 'BLOCK', reviewed_sha: first.sha, evidence: okEvidence, findings: [{ location: 'x:1', severity: 'blocker', problem: 'bad', fix: 'fix' }] }
      : { verdict: 'APPROVE', reviewed_sha: first.sha, evidence: okEvidence, findings: [] },
    'bootstrap-prepare:missing_mode_a': prepare,
    'bootstrap-cas-finalizer': finalize,
    'bootstrap-post-cas-audit': audit,
  }
  if (fix) {
    responses['bootstrap-fix:missing_mode_a'] = { report: final }
    responses['bootstrap-rereview:missing_mode_a'] = { verdict: 'APPROVE', reviewed_sha: final.sha, evidence: okEvidence, findings: [] }
    responses['bootstrap-cas-finalizer'] = casReceipt(final.sha)
    responses['bootstrap-post-cas-audit'] = auditReceipt(final.sha)
  }
  return responses
}

function readyResponses({ ratchet = ratchetPrepare(), measureValue = measure(), finalize = casReceipt(SHA.ratchet), audit = auditReceipt(SHA.ratchet) } = {}) {
  const responses = {
    capability: capability('ready'), measure: measureValue,
    attribute: { hypotheses: [{ id: 'H-1', target_metric_ids: ['mode_a_price_mae', 'mode_b_price_mae'], mechanism: 'engine', prediction: 'lower MAE', blast_radius: 'pricing', effort: 'S' }], new_suspect_candidates: [] },
    'ratchet-prepare': ratchet,
    'ratchet-post-decision-audit': audit,
  }
  if (ratchet.memory_verdict === 'ACCEPT') responses['ratchet-cas-finalizer'] = finalize
  return responses
}

const sprintPass = { passed: true, integration_branch: 'integration/run-1', integration_sha: SHA.integration, gate_evidence: okEvidence }

test('ready Improve binds deterministic caller identity into nested sprint', async () => {
  const { result, phases, workflowCalls } = await runOracle(readyResponses(), sprintPass)
  assert.equal(result.verdict, 'ACCEPT')
  assert.ok(phases.includes('Improve'))
  assert.deepEqual(workflowCalls.map(call => call.name), ['vol-sprint'])
  assert.equal(workflowCalls[0].args.base, SHA.measure)
  assert.equal(workflowCalls[0].args.run_key, 'run-1-improve')
  assert.match(workflowCalls[0].args.task, /Oracle RSI iter-001/)
  const replay = sprintFns.sprintRunId(SHA.measure, 'task-a', 'caller-a')
  assert.equal(replay, sprintFns.sprintRunId(SHA.measure, ' task-a ', ' caller-a '))
  assert.notEqual(replay, sprintFns.sprintRunId(SHA.measure, 'task-b', 'caller-a'))
  assert.notEqual(replay, sprintFns.sprintRunId(SHA.measure, 'task-a', 'caller-b'))
  assert.notEqual(replay, sprintFns.sprintRunId(SHA.base, 'task-a', 'caller-a'))
  assert.notEqual(
    sprintFns.sprintRunId(SHA.measure, 'task-a', ''),
    sprintFns.sprintRunId(SHA.measure, 'task-a', 'direct'),
  )
})

test('success evidence and APPROVE contracts reject exit failure/blockers', () => {
  assert.equal(oracleFns.validSuccessEvidence([{ command: 'test', exit_code: 1, output: 'failed' }]), false)
  assert.equal(oracleFns.validSuccessEvidence([{ command: 'ctest --preset rel-avx2 -L atx_vol_fast', exit_code: 0, output: 'PASS' }]), false)
  const review = { verdict: 'APPROVE', reviewed_sha: SHA.build, evidence: okEvidence, findings: [{ severity: 'blocker' }] }
  assert.equal(oracleFns.reviewContractError(review, SHA.build), 'APPROVE contains blocker')
  const broadReview = { verdict: 'APPROVE', reviewed_sha: SHA.build, evidence: [{ command: 'build\\bin\\atx-vol-tests.exe', exit_code: 0, output: 'PASS' }], findings: [] }
  assert.match(oracleFns.reviewContractError(broadReview, SHA.build), /invalid\/broad/)
})

test('actual bootstrap no-Fix path lands, while BLOCKED enters guarded abort cleanup', async () => {
  const { result, phases } = await runOracle(bootstrapResponses())
  assert.equal(result.verdict, 'BOOTSTRAP')
  assert.equal(result.canonical_after, SHA.build)
  assert.equal(phases.includes('Bootstrap Fix'), false)
  assert.deepEqual(phases.slice(-3), ['Bootstrap Verify', 'Bootstrap Finalize', 'Bootstrap Audit'])

  const blocked = blockedStage1Report()
  const cleanup = {
    passed: true, released: [blocked.lease_name],
    release_receipts: [leaseReceipt('release', leaseIdentity({ branch: blocked.branch, base: SHA.base, heartbeat: blocked.heartbeat_id }))],
    evidence: [{ command: 'powershell scripts\\lease-worktree.ps1 -Release pool-1 -RunId run-1', exit_code: 0, output: 'RELEASED pool-1 run-1' }],
  }
  const blockedRun = await runOracle({
    capability: capability('missing_data'), 'bootstrap-build:missing_data': { report: blocked }, 'bootstrap-abort-cleanup': cleanup,
  })
  assert.equal(blockedRun.result.verdict, 'FAILED')
  assert.match(blockedRun.result.failure, /bootstrap blocked: licensed ZIP unavailable/u)
  assert.equal(blockedRun.result.cleanup, cleanup)
  assert.deepEqual(blockedRun.result.cleanup.released, [blocked.lease_name])
  assert.deepEqual(blockedRun.calls, ['capability', 'bootstrap-build:missing_data', 'bootstrap-abort-cleanup'])

  for (const malformed of [blocked, { report: blocked, extra: true }, { report: null }]) {
    const malformedRun = await runOracle({ capability: capability('missing_data'), 'bootstrap-build:missing_data': malformed })
    assert.equal(malformedRun.result.verdict, 'FAILED')
    assert.match(malformedRun.result.failure, /StructuredOutput envelope invalid/u)
    assert.deepEqual(malformedRun.calls, ['capability', 'bootstrap-build:missing_data'])
  }

  const first = bootstrapReport()
  const fixCleanup = {
    passed: true, released: [first.lease_name],
    release_receipts: [leaseReceipt('release', leaseIdentity({ branch: first.branch, base: SHA.base, heartbeat: first.heartbeat_id }))],
    evidence: [{ command: 'powershell scripts\\lease-worktree.ps1 -Release pool-1 -RunId run-1', exit_code: 0, output: 'RELEASED pool-1 run-1' }],
  }
  const malformedFixResponses = bootstrapResponses({ fix: true })
  malformedFixResponses['bootstrap-fix:missing_mode_a'] = bootstrapReport(SHA.fixed)
  malformedFixResponses['bootstrap-abort-cleanup'] = fixCleanup
  const malformedFix = await runOracle(malformedFixResponses)
  assert.equal(malformedFix.result.verdict, 'FAILED')
  assert.match(malformedFix.result.failure, /StructuredOutput envelope invalid/u)
  assert.deepEqual(malformedFix.calls, [
    'capability', 'bootstrap-build:missing_mode_a', 'bootstrap-review:missing_mode_a',
    'bootstrap-fix:missing_mode_a', 'bootstrap-abort-cleanup',
  ])
})

test('actual bootstrap Fix path requires fresh re-review of new SHA before prepare', async () => {
  const { result, phases } = await runOracle(bootstrapResponses({ fix: true }))
  assert.equal(result.verdict, 'BOOTSTRAP')
  assert.equal(result.bootstrap.review.reviewed_sha, SHA.fixed)
  assert.ok(phases.indexOf('Bootstrap Re-review') < phases.indexOf('Bootstrap Verify'))
})

test('bootstrap missing gate/stale integration/release receipts fail before finalizer', async t => {
  for (const [name, mutate] of [
    ['missing gate', prepare => { prepare.gate_receipts.pop() }],
    ['extra gate', prepare => { prepare.gate_receipts.push(bootstrapGateReceipt('disk')) }],
    ['generic node gate', prepare => { prepare.gate_receipts[0].command = 'node mode_a_targeted_tests' }],
    ['spoofed gate output', prepare => { prepare.gate_receipts[0].output = 'mode_a_targeted_tests PASS' }],
    ['exit-zero ctest zero work', prepare => { const receipt = prepare.gate_receipts[0]; receipt.result.tests_executed = 0; receipt.result.tests_passed = 0; receipt.result.audit_summary = 'tests_executed=0 tests_passed=0'; receipt.output = JSON.stringify(receipt.result) }],
    ['exit-zero OracleBench zero rows', prepare => { const receipt = prepare.gate_receipts[1]; receipt.result.rows_processed = 0; receipt.result.audit_summary = `status=PASS rows_processed=0 metric_ids=${[...receipt.result.metric_ids].sort().join(',')}`; receipt.output = JSON.stringify(receipt.result) }],
    ['stale SHA', prepare => { prepare.integration_receipt.reviewed_sha = SHA.base }],
    ['missing release', prepare => { prepare.integration_release_receipt = null }],
  ]) await t.test(name, async () => {
    const { result, calls } = await runOracle(bootstrapResponses({ prepareMutator: mutate }))
    assert.equal(result.verdict, 'FAILED')
    assert.equal(calls.includes('bootstrap-cas-finalizer'), false)
  })
})

test('wrong-ref CAS fails and post-ref audit reports actual canonical truth', async () => {
  const responses = bootstrapResponses({ finalize: casReceipt(SHA.build, { ref: 'refs/heads/wrong' }), audit: auditReceipt(SHA.base) })
  const { result } = await runOracle(responses)
  assert.equal(result.verdict, 'FAILED')
  assert.equal(result.canonical_after, SHA.base)
  assert.match(result.failure, /CAS identity/)
})

test('bootstrap finalizer report loss audits a landed ref instead of assuming old SHA', async () => {
  const { result } = await runOracle(bootstrapResponses({ finalize: null, audit: auditReceipt(SHA.build) }))
  assert.equal(result.verdict, 'FAILED')
  assert.equal(result.canonical_after, SHA.build)
  assert.equal(result.landing_status, 'LANDED_AUDITED_WITH_INVALID_RECEIPT')
})

test('bootstrap finalizer exception still performs independent canonical audit', async () => {
  const responses = bootstrapResponses({ finalize: () => { throw new Error('finalizer transport lost') }, audit: auditReceipt(SHA.build) })
  const { result, calls } = await runOracle(responses)
  assert.equal(result.verdict, 'FAILED')
  assert.equal(result.canonical_after, SHA.build)
  assert.equal(calls.includes('bootstrap-post-cas-audit'), true)
  assert.match(result.failure, /finalizer transport lost/)
})

test('strict aggregate payload rejects unknown keys, row arrays, raw row content, and encoded blobs', () => {
  assert.equal(oracleFns.aggregatePayloadError(safePayload()), null)
  const variants = []
  variants.push({ ...safePayload(), raw_rows: [] })
  for (const raw of ['uPrc=100 rate=.05 sdiv=.01 de=.4 ga=.02', 'okey_xx=123 bidIV=.2 askIV=.3', Buffer.from('uPrc=100 rate=.05').toString('base64')]) {
    const prior = structuredClone(safePayload()); prior.prior_refuted_ids = [raw]; variants.push(prior)
    const suspect = structuredClone(safePayload()); suspect.oracle_suspect_cells = [raw]; variants.push(suspect)
    const prefixed = structuredClone(safePayload()); prefixed.oracle_suspect_cells = [`cell-${raw.replace(/[^A-Za-z0-9]/g, '')}`]; variants.push(prefixed)
    const convention = structuredClone(safePayload()); convention.conventions.rate_model = raw; variants.push(convention)
    const unit = structuredClone(safePayload()); unit.target_metrics[0].unit = raw; variants.push(unit)
  }
  const rows = structuredClone(safePayload()); rows.target_metrics = [[{ uPrc: 100, rate: 0.05 }]]; variants.push(rows)
  for (const payload of variants) assert.notEqual(oracleFns.aggregatePayloadError(payload), null)
})

test('actual workflow rejects plaintext or encoded row payloads before Analyst and holdout', async t => {
  for (const raw of ['uPrc=100 rate=.05 sdiv=.01 de=.4 ga=.02', 'okey_xx=123 bidIV=.2 askIV=.3', Buffer.from('okey_xx=123 bidIV=.2 askIV=.3').toString('base64')]) {
    await t.test(raw.slice(0, 16), async () => {
      const measured = measure()
      measured.analysis_context.conventions.rate_model = raw
      const { result, calls } = await runOracle({ capability: capability('ready'), measure: measured }, sprintPass)
      assert.equal(result.verdict, 'FAILED')
      assert.equal(calls.includes('attribute'), false)
      assert.equal(calls.includes('ratchet-prepare'), false)
    })
  }
})

test('actual ready Measure requires exact smoke/tune scorecards and quiet speed with no broad diagnostics', async t => {
  for (const [name, mutate] of [
    ['omitted scorecard', measured => { measured.evidence.shift() }],
    ['nonquiet speed', measured => { measured.gate_receipts[2].command = measured.gate_receipts[2].command.replace(' --quiet-host', '') }],
    ['broad diagnostic', measured => { measured.diagnostics = [{ command: 'ctest --preset dev', exit_code: 1, output: 'refused' }] }],
    ['self-reported forged baseline and pin', measured => {
      measured.gate_receipts[0].result.metrics[0].value = 2
      measured.gate_receipts[0].output = JSON.stringify(measured.gate_receipts[0].result)
      measured.evidence[0].output = measured.gate_receipts[0].output
      measured.attribution_payload = safePayload()
      measured.attribution_payload.target_metrics[0].baseline = 100
      measured.attribution_payload.speed.pin = 1
    }],
  ]) await t.test(name, async () => {
    const measured = measure(); mutate(measured)
    const { result, calls } = await runOracle({ capability: capability('ready'), measure: measured }, sprintPass)
    assert.equal(result.verdict, 'FAILED')
    assert.equal(calls.includes('attribute'), false)
  })
})

test('Measure baselines and speed pin are derived from exact typed command JSON', () => {
  const measured = measure()
  measured.gate_receipts[0].result.metrics[0].value = 2
  measured.gate_receipts[0].output = JSON.stringify(measured.gate_receipts[0].result)
  const payload = oracleFns.attributionPayloadFromMeasure(measured)
  assert.equal(payload.target_metrics.find(metric => metric.metric_id === 'mode_a_price_mae').baseline, 2)
  assert.equal(payload.speed.pin, 100)
  assert.equal(oracleFns.measurePayloadError(measured), null)
})

test('actual ready path workflow computes ACCEPT and exact CAS/audit', async () => {
  const { result } = await runOracle(readyResponses(), sprintPass)
  assert.equal(result.verdict, 'ACCEPT')
  assert.equal(result.ratchet.computed_verdict, 'ACCEPT')
  assert.equal(result.canonical_after, SHA.ratchet)
  assert.ok(result.ratchet_evidence.some(item => item.command.startsWith('git update-ref')))
  assert.ok(result.ratchet_evidence.some(item => item.command === 'verify holdout digest'))
  assert.ok(result.ratchet_evidence.some(item => item.command === 'atx-vol-oracle-bench --cohort holdout --market-check 7 --aggregate-only'))
  for (const metric of result.holdout.metrics) {
    assert.ok(result.ratchet_evidence.some(item => item.output.includes(metric.metric_id) && item.output.includes(String(metric.delta))))
  }
})

test('actual ready path workflow computes REJECT and never invokes finalizer', async () => {
  const reject = ratchetPrepare({ reject: true })
  const { result, calls } = await runOracle(readyResponses({ ratchet: reject, audit: auditReceipt(SHA.base) }), sprintPass)
  assert.equal(result.verdict, 'REJECT')
  assert.equal(result.canonical_after, SHA.base)
  assert.equal(calls.includes('ratchet-cas-finalizer'), false)
})

test('regressive target yields REJECT while inconsistent delta is contract FAILED', async () => {
  const reject = ratchetPrepare({ reject: true })
  assert.equal(oracleFns.computeRatchetVerdict(reject), 'REJECT')
  const inconsistent = ratchetPrepare()
  inconsistent.metrics[0].delta = 7
  const { result, calls } = await runOracle(readyResponses({ ratchet: inconsistent }), sprintPass)
  assert.equal(result.verdict, 'FAILED')
  assert.match(result.failure, /delta inconsistent/)
  assert.equal(calls.includes('ratchet-cas-finalizer'), false)
})

test('actual Ratchet rejects agent-selected target, direction, baseline, speed pin, gate command, and market spoofing', async t => {
  const attacks = [
    ['omitted target', value => { value.metrics.shift() }],
    ['price direction higher', value => { value.metrics.find(metric => metric.metric_id === 'mode_a_price_mae').direction = 'higher' }],
    ['price reclassified', value => { value.metrics.find(metric => metric.metric_id === 'mode_a_price_mae').gate = 'aggregate' }],
    ['baseline override', value => { value.metrics.find(metric => metric.metric_id === 'mode_a_price_mae').baseline = 1 }],
    ['candidate override', value => { const metric = value.metrics.find(metric => metric.metric_id === 'mode_a_price_mae'); metric.candidate = 1; metric.delta = 1 - metric.baseline }],
    ['speed pin override', value => { const speed = value.metrics.find(metric => metric.metric_id === SPEED_METRIC_ID); speed.candidate = 1; speed.delta = 1 - speed.baseline; speed.pin = 0 }],
    ['generic node gate', value => { value.gate_receipts[0].command = 'node holdout_mode_a'; value.gate_receipts[0].output = 'holdout_mode_a PASS' }],
    ['forged market side', value => { const market = value.market_evidence[0]; market.atx_price_mean = 104; market.oracle_price_mean = 100; market.atx_nbbo_distance = 0; market.oracle_nbbo_distance = 3; market.output = '7 99 101 104 100 0 3 100' }],
  ]
  for (const [name, mutate] of attacks) await t.test(name, async () => {
    const value = ratchetPrepare(); mutate(value)
    const { result, calls } = await runOracle(readyResponses({ ratchet: value }), sprintPass)
    assert.equal(result.verdict, 'FAILED')
    assert.equal(calls.includes('ratchet-cas-finalizer'), false)
  })
})

test('Ratchet missing digest, required gate, or suspect market evidence is FAILED', async t => {
  for (const [name, mutate] of [
    ['digest', value => { value.digest_receipt = null }],
    ['gate', value => { value.gate_receipts.pop() }],
    ['market', value => { value.market_evidence = [] }],
    ['broad success evidence', value => { value.evidence = [{ command: 'powershell scripts\\atx-build.ps1 build all', exit_code: 0, output: 'PASS' }] }],
  ]) await t.test(name, async () => {
    const value = ratchetPrepare()
    mutate(value)
    const { result, calls } = await runOracle(readyResponses({ ratchet: value }), sprintPass)
    assert.equal(result.verdict, 'FAILED')
    assert.equal(calls.includes('ratchet-cas-finalizer'), false)
  })
})

test('ready ACCEPT finalizer report loss audits actual landed canonical ref', async () => {
  const { result } = await runOracle(readyResponses({ finalize: null, audit: auditReceipt(SHA.ratchet) }), sprintPass)
  assert.equal(result.verdict, 'FAILED')
  assert.equal(result.canonical_after, SHA.ratchet)
  assert.equal(result.landing_status, 'LANDED_AUDITED_WITH_INVALID_RECEIPT')
})

test('ready ACCEPT finalizer exception still audits and reports actual landed canonical ref', async () => {
  const responses = readyResponses({ finalize: () => { throw new Error('finalizer transport lost') }, audit: auditReceipt(SHA.ratchet) })
  const { result, calls } = await runOracle(responses, sprintPass)
  assert.equal(result.verdict, 'FAILED')
  assert.equal(result.canonical_after, SHA.ratchet)
  assert.equal(calls.includes('ratchet-post-decision-audit'), true)
  assert.match(result.failure, /finalizer transport lost/)
})

test('sprint integration requires the exact mechanically-derived targeted registry without omission', () => {
  const identity = leaseIdentity({ branch: 'integration/run-1', base: SHA.base, heartbeat: 'run-1-integration' })
  const closure = [
    { file: 'atx-vol/src/pricing/american.cpp', unit_targets: ['atx-vol-tests'], unit_regexes: ['^AmericanGreeks.Delta_MatchesFd_Put$'], oracle_tests: ['american-rsi'], pch_off_targets: [] },
    { file: 'atx-vol/include/atx/vol/api/pricing/american.hpp', unit_targets: ['atx-vol-tests'], unit_regexes: ['^AmericanGreeks.Delta_MatchesFd_Put$'], oracle_tests: ['american-rsi'], pch_off_targets: ['atx-vol-tests'] },
  ]
  const gateRegistry = sprintFns.derivedGateRegistry(closure, true)
  const expected = { base_sha: SHA.base, run_id: 'run-1', branch: identity.branch, heartbeat_id: identity.heartbeat_id, reviewed_shas: [SHA.build], gate_registry: gateRegistry }
  const receipts = gateRegistry.map(item => ({ ...item, tested_sha: SHA.ratchet, exit_code: 0, output: sprintGateOutput(item) }))
  const gate = {
    passed: true, base_sha: SHA.base, lease_run_id: 'run-1', integration_branch: identity.branch, sha: SHA.ratchet,
    integrated_shas: [SHA.build], integration_worktree: identity.worktree, integration_lease: identity.lease_name,
    integration_heartbeat_id: identity.heartbeat_id, keeper_pid: identity.keeper_pid,
    keeper_process_started_utc: identity.keeper_process_started_utc, acquisition_receipt: leaseReceipt('acquire', identity),
    integration_receipts: [{ reviewed_sha: SHA.build, head_after: SHA.build, command: `git merge ${SHA.build}`, exit_code: 0, output: SHA.build }],
    head_receipt: auditReceipt(SHA.ratchet, { ref: 'HEAD' }), gate_receipts: receipts,
    release_receipt: leaseReceipt('release', identity),
    gate_results: receipts.map(receipt => ({ command: receipt.command, exit_code: 0, output: receipt.output })),
    leases_released: [identity.lease_name],
  }
  assert.equal(sprintFns.gateContractError(gate, expected), null)
  assert.match(sprintFns.gateContractError({ ...gate, integration_receipts: [] }, expected), /integration receipts/)
  assert.match(sprintFns.gateContractError({ ...gate, gate_receipts: gate.gate_receipts.slice(1) }, expected), /missing\/extra\/duplicated/)
  const duplicate = structuredClone(gate); duplicate.gate_receipts.push(structuredClone(receipts[0])); duplicate.gate_results.push({ command: receipts[0].command, exit_code: 0, output: receipts[0].output })
  assert.match(sprintFns.gateContractError(duplicate, expected), /missing\/extra\/duplicated/)
  const stale = structuredClone(gate); stale.gate_receipts[0].tested_sha = SHA.base
  assert.match(sprintFns.gateContractError(stale, expected), /required targeted gate/)
  const wrongCommand = structuredClone(gate); wrongCommand.gate_receipts[0].command += ' --extra'
  assert.match(sprintFns.gateContractError(wrongCommand, expected), /required targeted gate/)
  const missingSpeed = structuredClone(gate); const speedIndex = missingSpeed.gate_receipts.findIndex(item => item.gate_id === 'speed:rel_avx2_quiet'); missingSpeed.gate_receipts.splice(speedIndex, 1); missingSpeed.gate_results.splice(speedIndex, 1)
  assert.match(sprintFns.gateContractError(missingSpeed, expected), /missing\/extra\/duplicated/)
  const evidenceOmission = structuredClone(gate); evidenceOmission.gate_results.pop()
  assert.match(sprintFns.gateContractError(evidenceOmission, expected), /returned evidence|evidence count/)
  const evidenceExtra = structuredClone(gate); evidenceExtra.gate_results.push({ command: 'powershell scripts\\atx-build.ps1 -Preset dev -Ctest -R ^AnotherSuite$', exit_code: 0, output: 'PASS' })
  assert.match(sprintFns.gateContractError(evidenceExtra, expected), /evidence count/)
  const zeroUnitGate = structuredClone(gate)
  const unitReceipt = zeroUnitGate.gate_receipts.find(item => item.gate_id.startsWith('unit-test:'))
  const zeroTyped = JSON.parse(unitReceipt.output); zeroTyped.tests_executed = 0; zeroTyped.tests_passed = 0; zeroTyped.audit_summary = 'tests_executed=0 tests_passed=0'; unitReceipt.output = JSON.stringify(zeroTyped)
  zeroUnitGate.gate_results.find(item => item.command === unitReceipt.command).output = unitReceipt.output
  assert.match(sprintFns.gateContractError(zeroUnitGate, expected), /required targeted gate receipt invalid/)
  const broadDiagnostic = structuredClone(gate); broadDiagnostic.diagnostics = [{ command: 'ctest --preset dev', exit_code: 1, output: 'refused' }]
  assert.match(sprintFns.gateContractError(broadDiagnostic, expected), /broad or unanchored ctest/)
  assert.match(sprintFns.gateContractError({ ...gate, release_receipt: null }, expected), /release receipt/)
  assert.match(sprintFns.gateContractError({ ...gate, head_receipt: auditReceipt(SHA.base, { ref: 'HEAD' }) }, expected), /HEAD receipt/)
  const chained = structuredClone(gate)
  chained.integration_receipts[0].command += '; git reset --hard HEAD~1'
  assert.match(sprintFns.gateContractError(chained, expected), /integration receipt/)
})

test('oracle-loop command policy rejects full/broad work and derives scoped PCH-off only for changed headers', () => {
  for (const command of [
    'ctest --preset rel-avx2 -L atx_vol_fast --output-on-failure',
    'ctest --preset dev --output-on-failure',
    'powershell scripts\\atx-build.ps1 -Ctest',
    'powershell scripts\\atx-build.ps1 -Ctest -R .*',
    'cmake --build --preset hygiene',
    'powershell atx-vol\\ci\\run_all_gates.ps1',
    'build\\bin\\atx-vol-tests.exe',
    'build\\bin\\spx-wilmott-repro-tests.exe',
    'powershell scripts\\atx-build.ps1 build',
    'powershell scripts\\atx-build.ps1 build all',
    'node --test',
    'powershell scripts\\atx-build.ps1 -Preset dev -Ctest -R ^AmericanGreeks.Delta_MatchesFd_Put$; ctest --preset dev',
  ]) assert.notEqual(sprintFns.oracleLoopCommandError(command), null, command)
  assert.equal(sprintFns.oracleLoopCommandError('powershell scripts\\atx-build.ps1 -Preset dev -Ctest -R ^AmericanGreeks.Delta_MatchesFd_Put$'), null)
  assert.equal(sprintFns.oracleLoopCommandError('powershell scripts\\atx-build.ps1 -Preset hygiene build atx-vol-tests'), null)
  for (const command of ['build\\bin\\atx-vol-tests.exe', 'powershell scripts\\atx-build.ps1 build', 'powershell scripts\\atx-build.ps1 build all']) assert.notEqual(oracleFns.iterationCommandError(command), null, command)
  assert.equal(sprintFns.changedHeader('atx-vol/include/atx/vol/api/pricing/american.hpp'), true)
  assert.equal(sprintFns.changedHeader('atx-vol/src/pricing/american.cpp'), false)
  const cppOnly = sprintFns.derivedGateRegistry([{ file: 'atx-vol/src/pricing/american.cpp', unit_targets: ['atx-vol-tests'], unit_regexes: ['^AmericanGreeks.Delta_MatchesFd_Put$'], oracle_tests: ['american-rsi'], pch_off_targets: ['atx-vol-tests'] }])
  assert.equal(cppOnly.some(gate => gate.gate_id.startsWith('pch-off:')), false)
  assert.equal(cppOnly.find(gate => gate.gate_id.startsWith('unit-test:')).command, 'powershell scripts\\oracle-targeted-gate.ps1 -Gate sprint_american_greeks_delta_put')
  assert.ok(americanTests.includes('TEST(AmericanGreeks, Delta_MatchesFd_Put)'))
  assert.ok(adjustedGreeksTests.includes('TEST(AdjustedGreeks, FlatSmileLeavesDeltaUnchanged)'))
  assert.ok(settings.permissions.allow.includes('PowerShell(powershell scripts\\oracle-targeted-gate.ps1 -Gate sprint_american_greeks_delta_put)'))
  assert.ok(settings.permissions.allow.includes('PowerShell(powershell scripts\\oracle-targeted-gate.ps1 -Gate sprint_adjusted_greeks_flat_smile)'))
  const header = sprintFns.derivedGateRegistry([{ file: 'atx-vol/include/atx/vol/api/pricing/american.hpp', unit_targets: ['atx-vol-tests'], unit_regexes: ['^AmericanGreeks.Delta_MatchesFd_Put$'], oracle_tests: ['american-rsi'], pch_off_targets: ['atx-vol-tests'] }])
  assert.deepEqual(header.find(gate => gate.gate_id === 'pch-off:atx-vol-tests'), { gate_id: 'pch-off:atx-vol-tests', command: 'powershell scripts\\atx-build.ps1 -Preset hygiene build atx-vol-tests' })
  const validLane = { files_in_scope: ['atx-vol/src/pricing/american.cpp'], gate_closure: [{ file: 'atx-vol/src/pricing/american.cpp', unit_targets: ['atx-vol-tests'], unit_regexes: ['^AmericanGreeks.Delta_MatchesFd_Put$'], oracle_tests: ['american-rsi'], pch_off_targets: [] }] }
  assert.equal(sprintFns.closureError(validLane), null)
  const omitted = structuredClone(validLane); omitted.gate_closure = []
  assert.match(sprintFns.closureError(omitted), /every scoped file/)
  const broad = structuredClone(validLane); broad.gate_closure[0].unit_regexes = ['^.*$']
  assert.match(sprintFns.closureError(broad), /invalid\/broad/)
  const unknown = structuredClone(validLane); unknown.files_in_scope[0] = unknown.gate_closure[0].file = 'atx-vol/src/unowned/fake.cpp'
  assert.match(sprintFns.closureError(unknown), /not in workflow registry/)
  const unrelatedTarget = structuredClone(validLane); unrelatedTarget.gate_closure[0].unit_targets = ['fake-target']
  assert.match(sprintFns.closureError(unrelatedTarget), /does not match workflow path owner/)
  const unrelatedTest = structuredClone(validLane); unrelatedTest.gate_closure[0].oracle_tests = ['unrelated-rsi']
  assert.match(sprintFns.closureError(unrelatedTest), /does not match workflow path owner/)
  const substitution = structuredClone(validLane); substitution.gate_closure[0].unit_regexes = ['^AdjustedGreeks.FlatSmileLeavesDeltaUnchanged$']; substitution.gate_closure[0].oracle_tests = ['american-greeks-rsi']
  assert.match(sprintFns.closureError(substitution), /omitted mandatory workflow gates/)
  const lane = { ...validLane, id: 'american', branch: 'lane/american-run-1' }
  const identity = leaseIdentity({ branch: lane.branch, base: SHA.base, heartbeat: 'run-1-lane-american' })
  const laneGates = sprintFns.derivedGateRegistry(lane.gate_closure)
  const report = {
    outcome: 'DONE', lane_id: lane.id, branch: lane.branch, base_sha: SHA.base, sha: SHA.build,
    lease_run_id: 'run-1', lease_name: identity.lease_name, heartbeat_id: identity.heartbeat_id,
    worktree: identity.worktree, keeper_pid: identity.keeper_pid, keeper_process_started_utc: identity.keeper_process_started_utc,
    acquisition_receipt: leaseReceipt('acquire', identity), files_changed: lane.files_in_scope,
    evidence: laneGates.map(gate => ({ command: gate.command, exit_code: 0, output: sprintGateOutput(gate) })), diagnostics: [],
  }
  assert.equal(sprintFns.reportContractError(report, lane, SHA.base), null)
  const zeroTest = structuredClone(report)
  const unitEvidence = zeroTest.evidence.find(item => item.command.includes('sprint_american_greeks_delta_put'))
  const zeroResult = JSON.parse(unitEvidence.output); zeroResult.tests_executed = 0; zeroResult.tests_passed = 0; zeroResult.audit_summary = 'tests_executed=0 tests_passed=0'; unitEvidence.output = JSON.stringify(zeroResult)
  assert.match(sprintFns.reportContractError(zeroTest, lane, SHA.base), /missing exact changed-closure gates/)
  const extra = structuredClone(report); extra.evidence.push({ command: 'powershell scripts\\atx-build.ps1 -Preset dev -Ctest -R ^AnotherSuite$', exit_code: 0, output: 'PASS' })
  assert.match(sprintFns.reportContractError(extra, lane, SHA.base), /exact changed-closure command set/)
  const broadLane = structuredClone(report); broadLane.diagnostics = [{ command: 'ctest --preset dev -L atx_vol_fast', exit_code: 1, output: 'refused' }]
  assert.match(sprintFns.reportContractError(broadLane, lane, SHA.base), /label\/full regression/)
  assert.ok(sprint.includes("receipt.tested_sha !== gate.sha"))
  assert.ok(sprint.includes('gate.gate_results.length !== expected.gate_registry.length'))
})

test('analyst has no tools and Attribute receives only JSON typed payload', () => {
  const frontmatter = analyst.slice(0, analyst.indexOf('---', 3) + 3)
  const toolsLine = frontmatter.split(/\r?\n/).find(line => line.startsWith('tools:'))
  assert.deepEqual(JSON.parse(toolsLine.slice('tools:'.length).trim()), [])
  const attribute = oracle.slice(oracle.indexOf("phase('Attribute')"), oracle.indexOf("phase('Improve')"))
  for (const forbidden of ['scorecard_path', 'holdout_digest_receipt', 'holdout.sha256', 'NORTHSTAR.md', 'LEDGER.md', 'args.focus', 'FOCUS']) assert.equal(attribute.includes(forbidden), false)
})

test('capability inspection is tool-restricted to an aggregate-only fixed probe', () => {
  const frontmatter = capabilityAgent.slice(0, capabilityAgent.indexOf('---', 3) + 3)
  assert.match(frontmatter, /^tools: PowerShell$/m)
  assert.match(frontmatter, /^permissionMode: dontAsk$/m)
  assert.ok(oracle.includes("agentType: 'vol-capability-inspector'"))
  assert.ok(oracle.includes('Run exactly powershell scripts\\\\oracle-capability.ps1'))
  assert.ok(settings.permissions.allow.includes('PowerShell(powershell scripts\\oracle-capability.ps1)'))
  assert.equal(settings.permissions.allow.includes('PowerShell(powershell scripts\\oracle-capability.ps1*)'), false)
  assert.equal(/Get-Content|ReadAllLines|scan_parquet|Import-Csv/i.test(capabilityProbe), false)
  assert.match(capabilityProbe, /ReadAllText\(\$path\) \| ConvertFrom-Json/)
  assert.ok(capabilityProbe.includes('Get-CommittedJson $Sha $holdoutPath'))
  assert.ok(capabilityProbe.includes('Get-CohortMembershipDigest $holdout'))
  assert.ok(capabilityProbe.includes('Test-Disjoint $tune.underliers $holdout.underliers'))
  assert.ok(capabilityProbe.includes("$oracleRoot + '/bootstrap/data.json'"))
  assert.ok(capabilityProbe.includes("$oracleRoot + '/bootstrap/mode-a.json'"))
  assert.ok(capabilityProbe.includes("$oracleRoot + '/bootstrap/conventions.json'"))
  assert.ok(capabilityProbe.includes("$oracleRoot + '/bootstrap/mode-' + $slug + '.json'"))
  assert.ok(capabilityProbe.includes('Test-Provenance'))
  assert.equal(/holdout_membership\s*=|licensed_rows|raw_rows/i.test(capabilityProbe.slice(capabilityProbe.indexOf('$probeOutput'))), false)
})

test('bootstrap uses the fixed production targeted-gate adapter with typed receipts only', () => {
  for (const gateId of ['mode_a_targeted_tests', 'mode_a_smoke', 'convention_tests', 'mode_a_smoke_tune', 'residual_floor', 'mode_b_targeted_tests', 'mode_b_smoke_tune']) {
    const command = `powershell scripts\\oracle-targeted-gate.ps1 -Gate ${gateId}`
    assert.equal(BOOTSTRAP_GATE_COMMANDS[gateId], command)
    assert.ok(settings.permissions.allow.includes(`PowerShell(${command})`))
  }
  assert.ok(targetedGate.includes('Invoke-OracleTargetedGate'))
  assert.ok(targetedGate.includes('raw_output_sha256'))
  assert.ok(targetedGate.includes("'-R', '^mode_a_targeted_tests$', '--no-tests=error'"))
  assert.ok(targetedGate.includes("'--cohort', 'smoke,tune'"))
  assert.ok(targetedGate.includes('tests_executed'))
  assert.ok(targetedGate.includes('rows_processed'))
  assert.ok(targetedGate.includes('metric_ids'))
  assert.equal(/-L\s+atx_vol_fast|run_all_gates|build\s+all/i.test(targetedGate), false)
})

test('lease source starts independent keeper and rejects before/after-only ownership', () => {
  for (const marker of ['Start-HeartbeatKeeper', 'keeper_process_started_utc', 'keeper_ready_utc', 'authenticated ready signal', 'Stop-HeartbeatKeeper', 'KEEPER_STOPPED', 'Test-ExactProcessAlive']) assert.ok(leases.includes(marker), marker)
  assert.ok(keeper.includes('while ($true)'))
  assert.ok(keeper.includes('SetLastWriteTimeUtc'))
  assert.ok(keeper.includes('$readyPath'))
  assert.equal(leases.includes('[string]$Pulse'), false)
  assert.equal(oracle.includes('Pulse before/after'), false)
  assert.equal(sprint.includes('Pulse before/after'), false)
})

test('workflow source separates prepare, minimal finalizer, and independent audit', () => {
  for (const phase of ['Bootstrap Verify', 'Bootstrap Finalize', 'Bootstrap Audit', 'Ratchet Prepare', 'Ratchet Finalize', 'Ratchet Audit']) assert.ok(oracle.includes(`phase('${phase}')`), phase)
  assert.ok(oracle.includes('computeRatchetVerdict(ratchet)'))
  assert.ok(oracle.includes('git update-ref ${CANONICAL_REF} ${ratchet.ratchet_sha} ${BASE_SHA}'))
})
