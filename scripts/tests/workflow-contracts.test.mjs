import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'

const read = relative => readFileSync(resolve(process.cwd(), relative), 'utf8')
const oracle = read('.claude/workflows/vol-oracle-iter.js')
const sprint = read('.claude/workflows/vol-sprint.js')
const analyst = read('.claude/agents/vol-analyst.md')
const leases = read('scripts/lease-worktree.ps1')

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

function loadFunctions(source, names) {
  const declarations = names.map(name => extractFunction(source, name)).join('\n')
  return Function(`${declarations}; return { ${names.join(', ')} }`)()
}

const sprintFns = loadFunctions(sprint, [
  'validSuccessEvidence', 'evidenceReferencesTarget', 'reviewContractError',
  'laneFailureReason', 'gateContractError',
])
const oracleFns = loadFunctions(oracle, [
  'validSuccessEvidence', 'reviewContractError', 'bootstrapContractError', 'attributionPayloadError',
  'ratchetContractError',
])
const okEvidence = [{ command: 'verify target', exit_code: 0, output: 'PASS' }]

test('success evidence behavior rejects any nonzero exit code', () => {
  assert.equal(sprintFns.validSuccessEvidence(okEvidence), true)
  assert.equal(sprintFns.validSuccessEvidence([{ command: 'test', exit_code: 1, output: 'failed' }]), false)
  assert.equal(oracleFns.validSuccessEvidence([{ command: 'test', exit_code: 2, output: 'diagnostic' }]), false)
  assert.equal(oracleFns.validSuccessEvidence([]), false)
})

test('required-check evidence behavior rejects prose/echo mentions and accepts real commands', () => {
  const target = 'atx-vol/src/american.cpp'
  assert.equal(sprintFns.evidenceReferencesTarget([{ command: `echo ${target}`, exit_code: 0, output: target }], target), false)
  assert.equal(sprintFns.evidenceReferencesTarget([{ command: 'powershell scripts\\atx-build.ps1 check atx-vol\\src\\american.cpp', exit_code: 0, output: 'PASS' }], target), true)
})

test('review behavior rejects APPROVE+blocker and stale SHA', () => {
  const approveWithBlocker = {
    verdict: 'APPROVE', reviewed_sha: 'a'.repeat(40), evidence: okEvidence,
    findings: [{ severity: 'blocker', location: 'x:1', problem: 'bad', fix: 'fix' }],
  }
  assert.equal(sprintFns.reviewContractError(approveWithBlocker, { sha: 'a'.repeat(40) }), 'APPROVE verdict contains a blocker finding')
  assert.equal(oracleFns.reviewContractError(approveWithBlocker, 'a'.repeat(40)), 'APPROVE contains blocker')
  const clean = { verdict: 'APPROVE', reviewed_sha: 'b'.repeat(40), evidence: okEvidence, findings: [] }
  assert.equal(oracleFns.reviewContractError(clean, 'a'.repeat(40)), 'reviewed SHA mismatch')
})

test('mandatory lane behavior fails incomplete, blocked, and non-approved lanes before integration', () => {
  const lane = { id: 'pricing' }
  assert.equal(sprintFns.laneFailureReason(null, lane), 'pricing: incomplete pipeline')
  assert.equal(sprintFns.laneFailureReason({ contract_error: 'bad evidence' }, lane), 'pricing: bad evidence')
  const report = { outcome: 'DONE', sha: 'a'.repeat(40) }
  assert.equal(sprintFns.laneFailureReason({ report, review: { verdict: 'BLOCK', reviewed_sha: report.sha } }, lane), 'pricing: final review not APPROVE')
  assert.equal(sprintFns.laneFailureReason({ report, review: { verdict: 'APPROVE', reviewed_sha: 'b'.repeat(40) } }, lane), 'pricing: final review is stale')
  assert.equal(sprintFns.laneFailureReason({ report, review: { verdict: 'APPROVE', reviewed_sha: report.sha } }, lane), null)
})

test('integration gate behavior requires isolated lease and exact reviewed SHA list', () => {
  const sha = 'a'.repeat(40)
  const expected = {
    base_sha: 'b'.repeat(40), run_id: 'run-1', branch: 'integration/run-1',
    heartbeat_id: 'run-1-integration', reviewed_shas: [sha],
  }
  const gate = {
    passed: true, base_sha: expected.base_sha, lease_run_id: expected.run_id,
    integration_branch: expected.branch, sha: 'c'.repeat(40), integrated_shas: [sha],
    integration_worktree: 'C:\\atx-wt\\pool-3', integration_lease: 'pool-3',
    integration_heartbeat_id: expected.heartbeat_id, leases_released: ['pool-3'], gate_results: okEvidence,
  }
  assert.equal(sprintFns.gateContractError(gate, expected), null)
  assert.equal(sprintFns.gateContractError({ ...gate, integrated_shas: [] }, expected), 'integrated SHA list mismatch')
  assert.equal(sprintFns.gateContractError({ ...gate, integration_worktree: 'C:\\atx' }, expected), 'integration lease/worktree mismatch')
  assert.equal(sprintFns.gateContractError({ ...gate, gate_results: [{ command: 'test', exit_code: 1, output: 'failed' }] }, expected), 'integration gate lacks successful evidence')
})

test('bootstrap behavior requires review, scoped verifier, exact integration, and state advance', () => {
  const sha = 'a'.repeat(40)
  const digest = 'd'.repeat(64)
  const expected = {
    state: 'missing_mode_a', branch: 'lane/run', base_sha: 'b'.repeat(40),
    integration_branch: 'integration/run', canonical_ref: 'refs/heads/oracle/canonical',
    canonical_expected_old: 'b'.repeat(40), next_state: 'missing_conventions',
    run_id: 'run-1', heartbeat_id: 'run-1-bootstrap-mode-a',
    integration_heartbeat_id: 'run-1-bootstrap-integration', holdout_digest_receipt: digest,
  }
  const report = {
    outcome: 'DONE', state: expected.state, branch: expected.branch, base_sha: expected.base_sha,
    sha, lease_name: 'pool-1', lease_run_id: expected.run_id,
    heartbeat_id: expected.heartbeat_id, holdout_digest_receipt: digest,
    worktree: 'C:\\atx-wt\\pool-1', evidence: okEvidence,
  }
  const review = { verdict: 'APPROVE', reviewed_sha: sha, evidence: okEvidence, findings: [] }
  const gate = {
    passed: true, reviewed_sha: sha, integration_sha: sha,
    canonical_after: sha, integration_branch: expected.integration_branch,
    canonical_ref: expected.canonical_ref, canonical_expected_old: expected.canonical_expected_old,
    next_state: expected.next_state, integration_lease: 'pool-2', lease_run_id: expected.run_id,
    integration_heartbeat_id: expected.integration_heartbeat_id,
    holdout_digest_receipt: digest,
    integration_worktree: 'C:\\atx-wt\\pool-2', leases_released: ['pool-1', 'pool-2'],
    evidence: [...okEvidence, { command: 'git update-ref refs/heads/oracle/canonical', exit_code: 0, output: 'updated' }],
  }
  assert.equal(oracleFns.bootstrapContractError(report, review, gate, expected), null)
  assert.equal(oracleFns.bootstrapContractError(report, null, gate, expected), 'missing review')
  assert.equal(oracleFns.bootstrapContractError(report, review, null, expected), 'scoped verifier missing/failed')
  assert.equal(oracleFns.bootstrapContractError(report, review, { ...gate, integration_sha: 'c'.repeat(40) }, expected), 'exact reviewed SHA not landed')
  assert.equal(oracleFns.bootstrapContractError(report, review, { ...gate, next_state: 'ready' }, expected), 'canonical CAS or state advance mismatch')
})

test('Ratchet behavior requires evidence/exact tested SHA and ACCEPT/REJECT landing semantics', () => {
  const expected = {
    tested_sha: 'a'.repeat(40), tested_branch: 'integration/run', ratchet_branch: 'lane/ratchet',
    holdout_digest: 'd'.repeat(64), canonical_ref: 'refs/heads/oracle/canonical',
    canonical_before: 'b'.repeat(40), run_id: 'run-1', heartbeat_id: 'run-1-ratchet',
  }
  const landed = 'c'.repeat(40)
  const benchOutput = 'mode_a_price_mae baseline=0.002 candidate=0.001 delta=-0.001'
  const base = {
    verdict: 'ACCEPT', evidence: [
      ...okEvidence,
      { command: 'atx-vol-oracle-bench --cohort holdout', exit_code: 0, output: benchOutput },
      { command: 'git update-ref refs/heads/oracle/canonical', exit_code: 0, output: 'updated' },
    ],
    tested_sha: expected.tested_sha, tested_branch: expected.tested_branch,
    base_sha: expected.tested_sha, lease_run_id: expected.run_id, heartbeat_id: expected.heartbeat_id,
    ratchet_branch: expected.ratchet_branch, holdout_digest_verified: expected.holdout_digest,
    canonical_ref: expected.canonical_ref, canonical_expected_old: expected.canonical_before,
    canonical_after: landed, landed_sha: landed, ratchet_sha: landed,
    lease_released: true, lease_name: 'pool-2', worktree: 'C:\\atx-wt\\pool-2',
    northstar_updated: true, ledger_appended: ['ratchet result'],
    holdout_summary: 'Mode A price MAE improved', oracle_suspects_excluded: [],
    headline_metric_deltas: [{
      metric: 'mode_a_price_mae', baseline: 0.002, candidate: 0.001,
      delta: -0.001, unit: 'price', evidence_index: 1,
    }],
  }
  assert.equal(oracleFns.ratchetContractError(base, expected), null)
  assert.equal(oracleFns.ratchetContractError({ ...base, evidence: [] }, expected), 'Ratchet evidence missing/failed')
  assert.equal(oracleFns.ratchetContractError({ ...base, tested_sha: 'e'.repeat(40) }, expected), 'Ratchet tested wrong integration')
  const noLanding = base.evidence.filter(item => !item.command.includes('git update-ref'))
  assert.equal(oracleFns.ratchetContractError({ ...base, evidence: noLanding }, expected), 'ACCEPT lacks atomic update-ref evidence')
  const reject = {
    ...base, verdict: 'REJECT',
    evidence: [okEvidence[0], { command: 'atx-vol-oracle-bench --cohort holdout', exit_code: 0, output: benchOutput }],
    canonical_after: expected.canonical_before, landed_sha: '',
  }
  assert.equal(oracleFns.ratchetContractError(reject, expected), null)
  assert.equal(oracleFns.ratchetContractError({ ...reject, canonical_after: landed }, expected), 'REJECT changed canonical base')
  const unsupportedMetric = { ...base, headline_metric_deltas: [{ ...base.headline_metric_deltas[0], candidate: 7 }] }
  assert.equal(oracleFns.ratchetContractError(unsupportedMetric, expected), 'Ratchet metric not supported by referenced output')
})

test('attribution payload behavior permits aggregates but rejects holdout reachability and row-like data', () => {
  assert.equal(oracleFns.attributionPayloadError('smoke price MAE 0.001; tune vol MAE 4 bp; hint: AmericanModel::price'), null)
  for (const invalid of [
    'holdout price MAE 0.001',
    'receipt sha256: abc',
    `commit ${'a'.repeat(40)}`,
    'C:\\oracle\\scorecard.json',
    'atx-vol/src/american.cpp',
    'date=2026-08-15/underlier=SPY',
    'srPrc=1.2345',
  ]) assert.equal(oracleFns.attributionPayloadError(invalid), 'payload exposes holdout/hash/path/row data')
})

test('analyst configuration behavior has no workspace/tool reach', () => {
  const frontmatter = analyst.slice(0, analyst.indexOf('---', 3) + 3)
  const toolsLine = frontmatter.split(/\r?\n/).find(line => line.startsWith('tools:'))
  const configuredTools = JSON.parse(toolsLine.slice('tools:'.length).trim())
  assert.deepEqual(configuredTools, [])
  assert.equal(configuredTools.some(tool => ['Read', 'Grep', 'Glob', 'Bash'].includes(tool)), false)
  const attributePrompt = oracle.slice(oracle.indexOf("phase('Attribute')"), oracle.indexOf("phase('Improve')"))
  for (const forbidden of ['scorecard_path', 'holdout_digest_receipt', 'holdout.sha256', 'git show', 'NORTHSTAR.md', 'LEDGER.md']) {
    assert.equal(attributePrompt.includes(forbidden), false, `analyst prompt reaches ${forbidden}`)
  }
})

test('bootstrap flow is one fixed implementation lane with independent gate stages', () => {
  const bootstrap = oracle.slice(oracle.indexOf("if (capability.state !== 'ready')"), oracle.indexOf("if (!capability.canonical_exists)"))
  assert.equal(bootstrap.includes('vol-planner'), false)
  assert.equal(bootstrap.includes("workflow('vol-sprint'"), false)
  for (const phase of ['Bootstrap Build', 'Bootstrap Review', 'Bootstrap Fix', 'Bootstrap Re-review', 'Bootstrap Verify']) {
    assert.equal(bootstrap.includes(`phase('${phase}')`), true, `missing ${phase}`)
  }
  assert.equal(bootstrap.includes('integration HEAD must equal it'), true)
  assert.equal(bootstrap.includes('next_state'), true)
})

test('capability freezes receipt without opening membership; only Ratchet recomputes it', () => {
  const capability = oracle.slice(oracle.indexOf("phase('Capability')"), oracle.indexOf("if (capability.state !== 'ready')"))
  assert.equal(capability.includes('MUST NOT open it'), true)
  assert.equal(capability.includes('read only the already committed holdout.sha256 digest receipt'), true)
  const ratchet = oracle.slice(oracle.indexOf("phase('Ratchet')"))
  assert.equal(ratchet.includes('recompute canonical holdout membership digest'), true)
})

test('ready flow passes structured exact integration and returns Ratchet evidence', () => {
  assert.equal(sprint.includes('integration_branch: gate ? gate.integration_branch : null'), true)
  assert.equal(sprint.includes('integration_sha: gate ? gate.sha : null'), true)
  assert.equal(sprint.includes('gate_evidence: gate ? gate.gate_results : []'), true)
  assert.equal(oracle.includes('tested_sha: sprint.integration_sha'), true)
  assert.equal(oracle.includes('ratchet_evidence: ratchetError ? [] : ratchet.evidence'), true)
})

test('branches and leases are run-unique with explicit heartbeat ownership', () => {
  assert.equal(sprint.includes('branch: `lane/${String(lane.id)'), true)
  assert.equal(sprint.includes('const INTEGRATION_BRANCH = `integration/${RUN_SLUG}`'), true)
  assert.equal(oracle.includes('`lane/oracle-ratchet-${RUN_SLUG}`'), true)
  assert.equal(sprint.includes('-HeartbeatId ${laneHeartbeatId(lane)}'), true)
  assert.equal(oracle.includes('-HeartbeatId ${ratchetHeartbeat}'), true)
})

test('lease source requires durable owner, atomic publication, corruption recovery, and base match', () => {
  for (const marker of [
    'OwnerPid+OwnerProcessStartedUtc or HeartbeatId',
    'short-lived lease launcher',
    '[System.IO.File]::Move($temp, $Path)',
    'RECOVERED_CORRUPT',
    'Assert-BranchHeadMatchesBase',
  ]) assert.equal(leases.includes(marker), true, `missing ${marker}`)
})
