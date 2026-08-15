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
  'validSuccessEvidence', 'validLeaseReceipt', 'validHeadReceipt', 'validGateReceipt', 'validIntegrationCommand', 'casReceiptError', 'auditReceiptError',
  'reviewContractError', 'bootstrapReportError', 'bootstrapPrepareError', 'aggregatePayloadError',
  'metricDeltaConsistent', 'relativeRegression', 'ratchetPrepareContractError', 'computeRatchetVerdict',
], `const RATCHET_GATE_IDS = ['holdout_mode_a', 'holdout_mode_b', 'rel_avx2_speed']`)
const sprintFns = loadFunctions(sprint, [
  'validSuccessEvidence', 'validLeaseReceipt', 'validHeadReceipt', 'validIntegrationCommand', 'evidenceReferencesTarget', 'reviewContractError',
  'laneFailureReason', 'gateContractError',
])

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
    gate_receipts: [gateReceipt('mode_a_targeted_tests'), gateReceipt('mode_a_smoke')],
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
    schema_version: 1, iteration: 'iter-001',
    metrics: [{ cell_id: 'A-price-near', mode: 'A', metric: 'price_mae', count: 100, mae: 0.01, rmse: 0.02, p95: 0.03, within_tolerance_rate: 0.9, unit: 'price' }],
    prior_refuted_ids: ['H-0'], oracle_suspect_cells: ['suspect-cell'],
    convention_summary: 'theta per day; vega per point', source_symbols: ['AmericanModel::price'],
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
  const metrics = [
    { metric_id: 'mode_a_target', mode: 'A', gate: 'target', direction: 'lower', baseline: 2, candidate: reject ? 3 : 1, delta: reject ? 1 : -1, pin: 0, unit: 'tick', evidence_index: 0 },
    { metric_id: 'mode_b_target', mode: 'B', gate: 'target', direction: 'lower', baseline: 3, candidate: 2, delta: -1, pin: 0, unit: 'tick', evidence_index: 1 },
    { metric_id: 'aggregate_mae', mode: 'ALL', gate: 'aggregate', direction: 'lower', baseline: 100, candidate: 101, delta: 1, pin: 0, unit: 'bp', evidence_index: 2 },
    { metric_id: 'rows_per_second', mode: 'ALL', gate: 'speed', direction: 'higher', baseline: 100, candidate: 110, delta: 10, pin: 100, unit: 'rows/s', evidence_index: 3 },
  ]
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
    gate_receipts: ['holdout_mode_a', 'holdout_mode_b', 'rel_avx2_speed'].map(gateReceipt),
    oracle_suspects_excluded: ['suspect-cell'],
    market_evidence: [{ cell_id: 'suspect-cell', market_sides_with: 'atx-vol', command: 'verify market suspect-cell', exit_code: 0, output: 'NBBO sides with atx-vol' }],
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
    attribute: { hypotheses: [{ id: 'H-1', target_cells: ['A-price-near'], modes: ['A', 'B'], mechanism: 'engine', prediction: 'lower MAE', blast_radius: 'pricing', effort: 'S' }], new_suspect_candidates: [] },
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
  const variants = [
    { ...safePayload(), raw_rows: [] },
    { ...safePayload(), metrics: [[1, 2, 3]] },
    { ...safePayload(), convention_summary: 'srPrc=1.23' },
    { ...safePayload(), convention_summary: 'A'.repeat(100) },
  ]
  for (const payload of variants) assert.notEqual(oracleFns.aggregatePayloadError(payload), null)
})

test('actual ready path workflow computes ACCEPT and exact CAS/audit', async () => {
  const { result } = await runOracle(readyResponses(), sprintPass)
  assert.equal(result.verdict, 'ACCEPT')
  assert.equal(result.ratchet.computed_verdict, 'ACCEPT')
  assert.equal(result.canonical_after, SHA.ratchet)
  assert.ok(result.ratchet_evidence.some(item => item.command.startsWith('git update-ref')))
  assert.ok(result.ratchet_evidence.some(item => item.command === 'verify holdout digest'))
  assert.ok(result.ratchet_evidence.some(item => item.command === 'verify market suspect-cell'))
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
  const expected = { base_sha: SHA.base, run_id: 'run-1', branch: identity.branch, heartbeat_id: identity.heartbeat_id, reviewed_shas: [SHA.build], gate_ids: ['atx-vol-tests', 'AmericanSuite'] }
  const gate = {
    passed: true, base_sha: SHA.base, lease_run_id: 'run-1', integration_branch: identity.branch, sha: SHA.ratchet,
    integrated_shas: [SHA.build], integration_worktree: identity.worktree, integration_lease: identity.lease_name,
    integration_heartbeat_id: identity.heartbeat_id, keeper_pid: identity.keeper_pid,
    keeper_process_started_utc: identity.keeper_process_started_utc, acquisition_receipt: leaseReceipt('acquire', identity),
    integration_receipts: [{ reviewed_sha: SHA.build, head_after: SHA.build, command: `git merge ${SHA.build}`, exit_code: 0, output: SHA.build }],
    head_receipt: auditReceipt(SHA.ratchet, { ref: 'HEAD' }), gate_receipts: [gateReceipt('atx-vol-tests'), gateReceipt('AmericanSuite')],
    release_receipt: leaseReceipt('release', identity),
    gate_results: [
      { command: 'powershell scripts\\atx-build.ps1 -Ctest -R atx-vol-tests', exit_code: 0, output: 'atx-vol-tests PASS' },
      { command: 'powershell scripts\\atx-build.ps1 -Ctest -R AmericanSuite', exit_code: 0, output: 'AmericanSuite PASS' },
    ],
    leases_released: [identity.lease_name],
  }
  assert.equal(sprintFns.gateContractError(gate, expected), null)
  assert.match(sprintFns.gateContractError({ ...gate, integration_receipts: [] }, expected), /integration receipts/)
  assert.match(sprintFns.gateContractError({ ...gate, gate_receipts: gate.gate_receipts.slice(1) }, expected), /atx-vol-tests/)
  assert.match(sprintFns.gateContractError({ ...gate, gate_receipts: gate.gate_receipts.slice(0, 1) }, expected), /AmericanSuite/)
  assert.match(sprintFns.gateContractError({ ...gate, release_receipt: null }, expected), /release receipt/)
  assert.match(sprintFns.gateContractError({ ...gate, head_receipt: auditReceipt(SHA.base, { ref: 'HEAD' }) }, expected), /HEAD receipt/)
  const chained = structuredClone(gate)
  chained.integration_receipts[0].command += '; git reset --hard HEAD~1'
  assert.match(sprintFns.gateContractError(chained, expected), /integration receipt/)
})

test('analyst has no tools and Attribute receives only JSON typed payload', () => {
  const frontmatter = analyst.slice(0, analyst.indexOf('---', 3) + 3)
  const toolsLine = frontmatter.split(/\r?\n/).find(line => line.startsWith('tools:'))
  assert.deepEqual(JSON.parse(toolsLine.slice('tools:'.length).trim()), [])
  const attribute = oracle.slice(oracle.indexOf("phase('Attribute')"), oracle.indexOf("phase('Improve')"))
  for (const forbidden of ['scorecard_path', 'holdout_digest_receipt', 'holdout.sha256', 'NORTHSTAR.md', 'LEDGER.md']) assert.equal(attribute.includes(forbidden), false)
})

test('capability inspection is tool-restricted to an aggregate-only fixed probe', () => {
  const frontmatter = capabilityAgent.slice(0, capabilityAgent.indexOf('---', 3) + 3)
  assert.match(frontmatter, /^tools: PowerShell$/m)
  assert.match(frontmatter, /^permissionMode: dontAsk$/m)
  assert.ok(oracle.includes("agentType: 'vol-capability-inspector'"))
  assert.ok(oracle.includes('Run exactly powershell scripts\\\\oracle-capability.ps1'))
  assert.ok(settings.permissions.allow.includes('PowerShell(powershell scripts\\oracle-capability.ps1)'))
  assert.equal(settings.permissions.allow.includes('PowerShell(powershell scripts\\oracle-capability.ps1*)'), false)
  assert.equal(/Get-Content|ReadAll(?:Text|Lines)|scan_parquet|Import-Csv/i.test(capabilityProbe), false)
  assert.equal((capabilityProbe.match(/\bgit\s+-C\s+\$repoRoot\s+show\b/g) || []).length, 1)
  assert.match(capabilityProbe, /show \(\$baseSha \+ ':' \+ \$digestPath\)/)
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
