import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'

const read = relative => readFileSync(resolve(process.cwd(), relative), 'utf8')
const oracle = read('.claude/workflows/vol-oracle-iter.js')
const sprint = read('.claude/workflows/vol-sprint.js')
const leases = read('scripts/lease-worktree.ps1')

test('oracle capability states are ordered data -> Mode A -> conventions -> Mode B -> ready', () => {
  const ordered = [
    '1 missing_data:',
    '2 missing_mode_a:',
    '3 missing_conventions:',
    '4 missing_mode_b:',
    '5 ready.',
  ].map(marker => oracle.indexOf(marker))
  assert.ok(ordered.every(index => index >= 0), `missing state marker: ${ordered}`)
  assert.deepEqual([...ordered].sort((a, b) => a - b), ordered)
})

test('each bootstrap run dispatches exactly one fixed lane and returns before ready Measure', () => {
  const start = oracle.indexOf("if (capability.state !== 'ready') {")
  const end = oracle.indexOf("phase('Measure')")
  assert.ok(start >= 0 && end > start)
  const bootstrap = oracle.slice(start, end)
  assert.equal((bootstrap.match(/await agent\(/g) || []).length, 1)
  assert.doesNotMatch(bootstrap, /workflow\('vol-sprint'/)
  assert.doesNotMatch(bootstrap, /vol-planner/)
  assert.match(bootstrap, /return \{[\s\S]*verdict: bootstrapPassed \? 'BOOTSTRAP' : 'FAILED'/)
})

test('bootstrap stages forbid holdout benchmark and route one state to one stage', () => {
  for (const [state, stage] of [
    ['missing_data', '1'],
    ['missing_mode_a', '2'],
    ['missing_conventions', '3'],
    ['missing_mode_b', '4'],
  ]) {
    assert.match(oracle, new RegExp(`${state}: \\{[\\s\\S]{0,120}stage: '${stage}'`))
  }
  assert.match(oracle, /missing_data:[\s\S]*NEVER run atx-vol-oracle-bench on holdout/)
  assert.match(oracle, /missing_mode_a:[\s\S]*NEVER benchmark holdout/)
  assert.match(oracle, /missing_conventions:[\s\S]*NEVER benchmark holdout/)
  assert.match(oracle, /missing_mode_b:[\s\S]*NEVER benchmark holdout/)
})

test('holdout hash is frozen at capability start and hidden from analyst', () => {
  assert.match(oracle, /const HOLDOUT_HASH = capability\.holdout_hash/)
  assert.match(oracle, /Before reading holdout, recompute canonical membership hash/)
  assert.match(oracle, /holdout_hash_verified === HOLDOUT_HASH/)
  assert.match(oracle, /You are forbidden from reading holdout manifests, membership, scorecards, or row data; the hash is not provided/)
})

test('ready Measure uses a run-owned lease and its committed SHA becomes sprint base', () => {
  assert.match(oracle, /lease-worktree\.ps1 -Branch \$\{measureBranch\} -Base \$\{BASE_SHA\} -Agent oracle-measure -RunId \$\{RUN_ID\} -MaxPool 20/)
  assert.match(oracle, /Write and commit scorecards\/iter-/)
  assert.match(oracle, /then release the measurement lease with -RunId/)
  assert.match(oracle, /workflow\('vol-sprint', \{ task, base: measure\.sha \}\)/)
})

test('failed or incomplete sprint stops before Ratchet with no REJECT', () => {
  const failure = oracle.indexOf('if (!sprint || !sprint.passed)')
  const ratchet = oracle.indexOf("phase('Ratchet')")
  assert.ok(failure >= 0 && ratchet > failure)
  const failureBranch = oracle.slice(failure, ratchet)
  assert.match(failureBranch, /verdict: 'FAILED'/)
  assert.match(failureBranch, /no holdout ran and the REJECT counter must not change/)
  assert.match(failureBranch, /return \{/)
})

test('sprint fixes are freshly re-reviewed at the new commit', () => {
  const fix = sprint.indexOf("phase: 'Fix'")
  const rereview = sprint.indexOf("phase: 'Re-review'")
  assert.ok(fix >= 0 && rereview > fix)
  assert.match(sprint, /FRESH RE-REVIEW after Fix/)
  assert.match(sprint, /reviewed_sha !== report\.sha/)
  assert.match(sprint, /reviewContractError\(review, state\.report\)/)
})

test('mandatory lane failure returns before any integration phase', () => {
  const failure = sprint.indexOf('if (failures.length)')
  const release = sprint.indexOf("phase('Release')")
  const gate = sprint.indexOf("phase('Gate')")
  assert.ok(failure >= 0 && release > failure && gate > release)
  const failureBranch = sprint.slice(failure, release)
  assert.match(failureBranch, /ABORT before integration/)
  assert.match(failureBranch, /integration: null/)
  assert.match(failureBranch, /return \{/)
})

test('approved lane leases are released before isolated integration acquisition', () => {
  const release = sprint.indexOf("phase('Release')")
  const gate = sprint.indexOf("phase('Gate')")
  assert.ok(release >= 0 && gate > release)
  const releaseBlock = sprint.slice(release, gate)
  assert.match(releaseBlock, /-Release <pool-N> -RunId/)
  assert.match(releaseBlock, /integration was not started/)
  const gateBlock = sprint.slice(gate)
  assert.match(gateBlock, /NEW isolated pool lease/)
  assert.match(gateBlock, /lease-worktree\.ps1 -Branch/)
  assert.match(gateBlock, /The returned C:\\\\atx-wt\\\\pool-N path is the ONLY place/)
  assert.match(gateBlock, /never use C:\\\\atx/)
})

test('workflow evidence contracts bind commands to required targets and reviews to SHAs', () => {
  assert.match(sprint, /const requiredReferences = \[\.\.\.lane\.check_targets, \.\.\.lane\.build_targets, \.\.\.lane\.suites\]/)
  assert.match(sprint, /evidence does not reference required checks/)
  assert.match(sprint, /reviewed stale SHA/)
  assert.match(sprint, /typeof item\.output === 'string' && item\.output\.trim\(\)/)
  assert.match(oracle, /typeof item\.output === 'string' && item\.output\.trim\(\)/)
})

test('lease implementation uses atomic records and v2 owner identity metadata', () => {
  assert.match(leases, /\[System\.IO\.FileMode\]::CreateNew/)
  for (const field of ['run_id', 'owner_pid', 'owner_process_started_utc', 'branch', 'acquired_utc']) {
    assert.match(leases, new RegExp(`${field} =`))
  }
  assert.match(leases, /TestPoolRoot/)
  assert.match(leases, /--git-common-dir/)
  assert.match(leases, /\[int\]\$MaxPool = 20/)
  assert.match(leases, /run_id mismatch:[\s\S]*refusing release/)
  assert.match(leases, /owner state is.*PID\/start guard/)
})
