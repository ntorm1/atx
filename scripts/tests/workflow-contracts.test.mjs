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
const capabilityAgent = read('.claude/agents/vol-capability-inspector.md')
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
  rel_avx2_speed: 'atx-vol-oracle-bench --cohort tune --benchmark-speed --preset rel-avx2 --aggregate-only',
}
const BOOTSTRAP_GATE_COMMANDS = {
  disk: 'powershell scripts\\oracle-bootstrap-preflight.ps1 -Gate disk',
  ingest_manifest: 'powershell scripts\\oracle-bootstrap-preflight.ps1 -Gate ingest_manifest',
  cohort_manifests: 'powershell scripts\\oracle-bootstrap-preflight.ps1 -Gate cohort_manifests',
  holdout_digest: 'powershell scripts\\oracle-bootstrap-preflight.ps1 -Gate holdout_digest',
  mode_a_targeted_tests: 'powershell scripts\\atx-build.ps1 -Ctest -R mode_a_targeted_tests',
  mode_a_smoke: 'atx-vol-oracle-bench --cohort smoke --mode A --aggregate-only',
  convention_tests: 'powershell scripts\\atx-build.ps1 -Ctest -R convention_tests',
  mode_a_smoke_tune: 'atx-vol-oracle-bench --cohort smoke,tune --mode A --aggregate-only',
  residual_floor: 'atx-vol-oracle-bench --cohort smoke,tune --mode A --residual-floor --aggregate-only',
  mode_b_targeted_tests: 'powershell scripts\\atx-build.ps1 -Ctest -R mode_b_targeted_tests',
  mode_b_smoke_tune: 'atx-vol-oracle-bench --cohort smoke,tune --mode B --aggregate-only',
}

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
  'validSuccessEvidence', 'validLeaseReceipt', 'validHeadReceipt', 'validGateReceipt', 'expectedGateMetricIds', 'validRatchetGateReceipt', 'validIntegrationCommand', 'casReceiptError', 'auditReceiptError',
  'reviewContractError', 'bootstrapReportError', 'bootstrapPrepareError', 'aggregatePayloadError',
  'metricDeltaConsistent', 'expectedRatchetMetrics', 'sameNumber', 'distanceToInterval', 'marketCommand', 'marketReceiptError', 'relativeRegression', 'ratchetPrepareContractError', 'computeRatchetVerdict',
], `const RATCHET_GATE_IDS = ['holdout_mode_a', 'holdout_mode_b', 'rel_avx2_speed']; const TARGET_REGISTRY = ${JSON.stringify(TARGET_REGISTRY)}; const AGGREGATE_REGISTRY = ${JSON.stringify(AGGREGATE_REGISTRY)}; const SPEED_METRIC_ID = '${SPEED_METRIC_ID}'; const RATCHET_GATE_COMMANDS = ${JSON.stringify(RATCHET_GATE_COMMANDS)}; const BOOTSTRAP_GATE_COMMANDS = ${JSON.stringify(BOOTSTRAP_GATE_COMMANDS)}`)
const sprintFns = loadFunctions(sprint, [
  'validSuccessEvidence', 'validLeaseReceipt', 'validHeadReceipt', 'validIntegrationCommand', 'evidenceReferencesTarget', 'isFullFastCommand', 'changedHeader', 'hygieneCommand', 'reviewContractError',
  'laneFailureReason', 'gateContractError',
], "const AUTHORITATIVE_FAST_GATE = 'atx_vol_fast'; const AUTHORITATIVE_FAST_COMMAND = 'ctest --preset rel-avx2 -L atx_vol_fast --output-on-failure'")

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
  const result = { schema_version: 1, status: 'PASS', observations: 100 }
  return { gate_id: gateId, command: BOOTSTRAP_GATE_COMMANDS[gateId], exit_code: 0, output: JSON.stringify(result), result }
}

function ratchetGateReceipt(gateId) {
  const metricIds = gateId === 'holdout_mode_a' ? TARGET_REGISTRY.filter(item => item.mode === 'A').map(item => item.metric_id)
    : gateId === 'holdout_mode_b' ? TARGET_REGISTRY.filter(item => item.mode === 'B').map(item => item.metric_id) : [SPEED_METRIC_ID]
  const result = { schema_version: 1, status: 'PASS', observations: 100, metric_ids: metricIds }
  return { gate_id: gateId, command: RATCHET_GATE_COMMANDS[gateId], exit_code: 0, output: JSON.stringify(result), result }
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

function measure() {
  const branch = 'lane/oracle-measure-iter-001-run-1'
  const identity = leaseIdentity({ branch, base: SHA.base, heartbeat: 'run-1-measure' })
  return {
    status: 'ok', iter: 'iter-001', scorecard_path: 'scorecard.json', branch, sha: SHA.measure, base_sha: SHA.base,
    worktree: identity.worktree, lease_name: identity.lease_name, lease_run_id: 'run-1', heartbeat_id: identity.heartbeat_id,
    keeper_pid: identity.keeper_pid, keeper_process_started_utc: identity.keeper_process_started_utc,
    acquisition_receipt: leaseReceipt('acquire', identity), release_receipt: leaseReceipt('release', identity),
    lease_released: true, attribution_payload: safePayload(), evidence: okEvidence,
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
    gate_receipts: ['holdout_mode_a', 'holdout_mode_b', 'rel_avx2_speed'].map(ratchetGateReceipt),
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
  source = source.replace(/const RUN_ID = `vol-oracle-\$\{Date\.now\(\)\}-\$\{Math\.random\(\)\.toString\(16\)\.slice\(2\)\}`/, "const RUN_ID = 'run-1'")
  const calls = []
  const phases = []
  const queues = Object.fromEntries(Object.entries(responses).map(([key, value]) => [key, Array.isArray(value) ? [...value] : [value]]))
  const agent = async (_prompt, options) => {
    calls.push(options.label)
    const queue = queues[options.label]
    assert.ok(queue && queue.length, `unexpected agent label ${options.label}`)
    const value = queue.shift()
    return typeof value === 'function' ? value() : value
  }
  const workflow = async () => sprintResult
  const phase = name => phases.push(name)
  const fn = new AsyncFunction('args', 'phase', 'agent', 'workflow', source)
  const result = await fn({}, phase, agent, workflow)
  return { result, phases, calls }
}

function bootstrapResponses({ fix = false, prepareMutator, finalize = casReceipt(SHA.build), audit = auditReceipt(SHA.build) } = {}) {
  const first = bootstrapReport()
  const final = fix ? bootstrapReport(SHA.fixed) : first
  const prepare = bootstrapPrepare(final)
  if (prepareMutator) prepareMutator(prepare)
  const responses = {
    capability: capability(),
    'bootstrap-build:missing_mode_a': first,
    'bootstrap-review:missing_mode_a': fix
      ? { verdict: 'BLOCK', reviewed_sha: first.sha, evidence: okEvidence, findings: [{ location: 'x:1', severity: 'blocker', problem: 'bad', fix: 'fix' }] }
      : { verdict: 'APPROVE', reviewed_sha: first.sha, evidence: okEvidence, findings: [] },
    'bootstrap-prepare:missing_mode_a': prepare,
    'bootstrap-cas-finalizer': finalize,
    'bootstrap-post-cas-audit': audit,
  }
  if (fix) {
    responses['bootstrap-fix:missing_mode_a'] = final
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

test('success evidence and APPROVE contracts reject exit failure/blockers', () => {
  assert.equal(oracleFns.validSuccessEvidence([{ command: 'test', exit_code: 1, output: 'failed' }]), false)
  const review = { verdict: 'APPROVE', reviewed_sha: SHA.build, evidence: okEvidence, findings: [{ severity: 'blocker' }] }
  assert.equal(oracleFns.reviewContractError(review, SHA.build), 'APPROVE contains blocker')
})

test('actual bootstrap no-Fix path prepares, finalizes exact CAS, audits, and returns BOOTSTRAP', async () => {
  const { result, phases } = await runOracle(bootstrapResponses())
  assert.equal(result.verdict, 'BOOTSTRAP')
  assert.equal(result.canonical_after, SHA.build)
  assert.equal(phases.includes('Bootstrap Fix'), false)
  assert.deepEqual(phases.slice(-3), ['Bootstrap Verify', 'Bootstrap Finalize', 'Bootstrap Audit'])
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
      measured.attribution_payload.conventions.rate_model = raw
      const { result, calls } = await runOracle({ capability: capability('ready'), measure: measured }, sprintPass)
      assert.equal(result.verdict, 'FAILED')
      assert.equal(calls.includes('attribute'), false)
      assert.equal(calls.includes('ratchet-prepare'), false)
    })
  }
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

test('sprint integration receipt contract rejects missing merge/gate/release and wrong HEAD', () => {
  const identity = leaseIdentity({ branch: 'integration/run-1', base: SHA.base, heartbeat: 'run-1-integration' })
  const hygiene = 'cmake --preset hygiene && cmake --build --preset hygiene --target atx-vol-tests'
  const expected = { base_sha: SHA.base, run_id: 'run-1', branch: identity.branch, heartbeat_id: identity.heartbeat_id, reviewed_shas: [SHA.build], gate_ids: ['atx-vol-tests', 'AmericanSuite', 'atx_vol_fast', 'hygiene_changed_closure'], hygiene_command: hygiene }
  const receipts = [
    { ...gateReceipt('atx-vol-tests'), tested_sha: SHA.ratchet },
    { ...gateReceipt('AmericanSuite'), tested_sha: SHA.ratchet },
    { gate_id: 'atx_vol_fast', tested_sha: SHA.ratchet, command: 'ctest --preset rel-avx2 -L atx_vol_fast --output-on-failure', exit_code: 0, output: '3275/3275 PASS' },
    { gate_id: 'hygiene_changed_closure', tested_sha: SHA.ratchet, command: hygiene, exit_code: 0, output: 'scoped hygiene PASS' },
  ]
  const gate = {
    passed: true, base_sha: SHA.base, lease_run_id: 'run-1', integration_branch: identity.branch, sha: SHA.ratchet,
    integrated_shas: [SHA.build], integration_worktree: identity.worktree, integration_lease: identity.lease_name,
    integration_heartbeat_id: identity.heartbeat_id, keeper_pid: identity.keeper_pid,
    keeper_process_started_utc: identity.keeper_process_started_utc, acquisition_receipt: leaseReceipt('acquire', identity),
    integration_receipts: [{ reviewed_sha: SHA.build, head_after: SHA.build, command: `git merge ${SHA.build}`, exit_code: 0, output: SHA.build }],
    head_receipt: auditReceipt(SHA.ratchet, { ref: 'HEAD' }), gate_receipts: receipts,
    release_receipt: leaseReceipt('release', identity),
    gate_results: [
      { command: 'powershell scripts\\atx-build.ps1 -Ctest -R atx-vol-tests', exit_code: 0, output: 'atx-vol-tests PASS' },
      { command: 'powershell scripts\\atx-build.ps1 -Ctest -R AmericanSuite', exit_code: 0, output: 'AmericanSuite PASS' },
      { command: 'ctest --preset rel-avx2 -L atx_vol_fast --output-on-failure', exit_code: 0, output: '3275/3275 PASS' },
      { command: hygiene, exit_code: 0, output: 'scoped hygiene PASS' },
    ],
    leases_released: [identity.lease_name],
  }
  assert.equal(sprintFns.gateContractError(gate, expected), null)
  assert.match(sprintFns.gateContractError({ ...gate, integration_receipts: [] }, expected), /integration receipts/)
  assert.match(sprintFns.gateContractError({ ...gate, gate_receipts: gate.gate_receipts.slice(1) }, expected), /missing\/extra\/duplicated/)
  const duplicateFast = structuredClone(gate); duplicateFast.gate_receipts.push(structuredClone(receipts[2])); duplicateFast.gate_results.push({ command: receipts[2].command, exit_code: 0, output: receipts[2].output })
  assert.match(sprintFns.gateContractError(duplicateFast, expected), /missing\/extra\/duplicated/)
  const staleFast = structuredClone(gate); staleFast.gate_receipts[2].tested_sha = SHA.base
  assert.match(sprintFns.gateContractError(staleFast, expected), /atx_vol_fast/)
  const broadHygiene = structuredClone(gate); broadHygiene.gate_receipts[3].command = 'cmake --preset hygiene && cmake --build --preset hygiene'
  assert.match(sprintFns.gateContractError(broadHygiene, expected), /hygiene_changed_closure/)
  assert.match(sprintFns.gateContractError({ ...gate, release_receipt: null }, expected), /release receipt/)
  assert.match(sprintFns.gateContractError({ ...gate, head_receipt: auditReceipt(SHA.base, { ref: 'HEAD' }) }, expected), /HEAD receipt/)
  const chained = structuredClone(gate)
  chained.integration_receipts[0].command += '; git reset --hard HEAD~1'
  assert.match(sprintFns.gateContractError(chained, expected), /integration receipt/)
})

test('sprint mechanically reserves full fast for one post-freeze gate and scopes hygiene to changed headers', () => {
  assert.equal(sprintFns.isFullFastCommand('ctest -L atx_vol_fast'), true)
  assert.equal(sprintFns.isFullFastCommand('ctest -R AmericanSuite'), false)
  assert.equal(sprintFns.changedHeader('atx-vol/include/model.hpp'), true)
  assert.equal(sprintFns.changedHeader('atx-vol/src/model.cpp'), false)
  assert.equal(sprintFns.hygieneCommand(['AmericanSuite']), 'cmake --preset hygiene && cmake --build --preset hygiene --target AmericanSuite')
  assert.ok(sprint.includes('lane attempted forbidden full atx_vol_fast gate'))
  assert.ok(sprint.includes("receipt.tested_sha !== gate.sha"))
  assert.ok(sprint.includes('gate.gate_results.length !== expected.gate_ids.length'))
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
  assert.equal(/ReadAllText\([^)]*holdout/i.test(capabilityProbe), false)
  assert.ok(capabilityProbe.includes("$oracleRoot + '/bootstrap/data.json'"))
  assert.ok(capabilityProbe.includes("$oracleRoot + '/bootstrap/mode-a.json'"))
  assert.ok(capabilityProbe.includes("$oracleRoot + '/bootstrap/conventions.json'"))
  assert.ok(capabilityProbe.includes("$oracleRoot + '/bootstrap/mode-' + $slug + '.json'"))
  assert.ok(capabilityProbe.includes('Test-Provenance'))
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
