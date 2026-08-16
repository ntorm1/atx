import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'

const read = relative => readFileSync(resolve(process.cwd(), relative), 'utf8')
const oracle = read('.claude/workflows/vol-oracle-iter.js')
const sprint = read('.claude/workflows/vol-sprint.js')
const settings = JSON.parse(read('.claude/settings.json'))
const forbidden = 'disallowedTools: Bash, PowerShell, Edit, Write, NotebookEdit, EnterWorktree'

function agentContract(name) {
  const source = read(`.claude/agents/${name}.md`)
  assert.ok(source.split(/\r?\n/).includes(forbidden), `${name} lacks the exact deny list`)
  assert.match(source, /^tools: .+$/m, `${name} lacks an explicit nonempty tool list`)
  assert.match(source, /^permissionMode: dontAsk$/m)
  return source
}

function compileWorkflow(source) {
  const body = source.replace('export const meta', 'const meta')
  return new Function('args', 'agent', 'phase', 'workflow', 'pipeline', `return (async () => {\n${body}\n})()`)
}

function rootGuard(canonicalSha) {
  return {
    main_sha: 'b'.repeat(40), canonical_sha: canonicalSha,
    index_sha256: '1'.repeat(64), tracked_sha256: '2'.repeat(64),
    untracked_sha256: '3'.repeat(64), raw_sha256: '4'.repeat(64),
  }
}

function brokerEvidence(operation, canonicalSha) {
  const guard = rootGuard(canonicalSha)
  return {
    logical_operation: operation, physical_cwd: 'C:\\atx', command: 'fixed broker operation',
    exit_code: 0, output: 'PASS', raw_output_sha256: '5'.repeat(64),
    root_guard_before: guard, root_guard_after: guard,
  }
}

test('standalone vol-sprint is a typed fail-closed workflow with no dispatch', async () => {
  let calls = 0
  const result = await compileWorkflow(sprint)({ task: 'oracle improvement' }, async () => { calls += 1 }, () => {}, async () => {}, async () => {})
  assert.deepEqual(result, {
    passed: false, blocked: ['ORACLE_BROKER_MIGRATION_REQUIRED'], failure: 'ORACLE_BROKER_MIGRATION_REQUIRED',
    integration_branch: null, integration_sha: null, gate_evidence: [],
  })
  assert.equal(calls, 0)
  assert.doesNotMatch(sprint, /\bagent\s*\(/)
  assert.doesNotMatch(sprint, /\bworkflow\s*\(/)
})

test('ready oracle capability fails closed before Measure, Sprint, Ratchet, or holdout', async () => {
  const base = 'b'.repeat(40)
  const labels = []
  const agent = async (_prompt, options) => {
    labels.push(options.label)
    assert.equal(options.agentType, 'vol-capability-inspector')
    return {
      state: 'ready', canonical_ref: 'refs/heads/oracle/canonical', canonical_exists: true,
      base_ref: 'refs/heads/oracle/canonical', base_sha: base, holdout_digest_receipt: 'd'.repeat(64), next_iter: 'iter-1',
      evidence: [{ command: 'powershell scripts\\oracle-capability.ps1', exit_code: 0, output: 'state=ready\ncanonical_exists=true' }],
      broker_evidence: brokerEvidence('capability_probe', base),
    }
  }
  const result = await compileWorkflow(oracle)({}, agent, () => {}, async () => { throw new Error('nested workflow reached') }, async () => {})
  assert.equal(result.failure, 'READY_BROKER_MIGRATION_REQUIRED')
  assert.equal(result.verdict, 'FAILED')
  assert.equal(result.holdout, null)
  assert.deepEqual(labels, ['capability'])
})

test('Stage 1 recovery and integration agents have operation-specific broker tools', () => {
  const recovery = agentContract('vol-stage1-recovery')
  assert.match(recovery, /^tools: mcp__oracle_lane_broker__recover_stage1$/m)
  const builder = agentContract('vol-builder')
  const builderTools = builder.match(/^tools: .+$/m)[0]
  assert.doesNotMatch(builderTools, /recover_stage1|lane_integrate|lane_release|lane_quarantine/)
  const verifier = agentContract('vol-verifier')
  assert.match(verifier, /^tools: mcp__oracle_lane_broker__lane_integrate, mcp__oracle_lane_broker__gate_run$/m)
  assert.doesNotMatch(verifier, /patch_apply|lane_commit|lane_release|canonical_finalize/)
  assert.match(agentContract('vol-lane-opener'), /^tools: mcp__oracle_lane_broker__lane_open$/m)
  assert.match(agentContract('vol-lane-releaser'), /^tools: mcp__oracle_lane_broker__lane_release$/m)
  assert.match(agentContract('vol-stage1-quarantiner'), /^tools: mcp__oracle_lane_broker__lane_quarantine$/m)
  const finalizer = agentContract('vol-ref-finalizer')
  assert.match(finalizer, /^tools: mcp__oracle_lane_broker__canonical_finalize$/m)
})

test('active bootstrap workflow owns acquisition, quarantine, immutable verify, and exact SHA/tree finalization', () => {
  const active = oracle.slice(0, oracle.indexOf('/* RETIRED_READY_PATH'))
  assert.match(active, /buildAgentType = capability\.state === 'missing_data' \? 'vol-stage1-recovery' : 'vol-builder'/)
  assert.match(active, /label: 'bootstrap-build-quarantine'/)
  assert.match(active, /agentType: 'vol-stage1-quarantiner', schema: BROKER_QUARANTINE/)
  assert.match(active, /integrationRelease\.sha !== report\.sha \|\| integrationRelease\.tree !== report\.tree/)
  assert.match(active, /expected_sha=\$\{report\.sha\}, expected_tree=\$\{report\.tree\}/)
  assert.doesNotMatch(active, /Run exactly git update-ref/)
})

test('project settings remove direct oracle shell and canonical-ref approvals', () => {
  assert.equal(settings.enableAllProjectMcpServers, true)
  assert.deepEqual(settings.enabledMcpjsonServers, ['oracle_lane_broker'])
  assert.ok(settings.permissions.allow.includes('mcp__oracle_lane_broker__*'))
  const approvals = settings.permissions.allow.join('\n')
  assert.doesNotMatch(approvals, /oracle-(?:capability|adopt|bootstrap|targeted)/)
  assert.doesNotMatch(approvals, /update-ref refs\/heads\/oracle\/canonical/)
})

test('lease manager preserves quarantined lanes and excludes them from allocation', () => {
  const lease = read('scripts/lease-worktree.ps1')
  assert.match(lease, /if \(\$Quarantine\)/)
  assert.match(lease, /Stop-HeartbeatKeeper \$record \$wtRoot -RemoveFiles/)
  assert.match(lease, /\.atx-quarantine-v3/)
  assert.match(lease, /\$leaseMissing -and \$quarantineMissing/)
  assert.doesNotMatch(lease.slice(lease.indexOf('if ($Quarantine)'), lease.indexOf('if ($Release)')), /git\s+-C|reset|clean|checkout|restore/)
})
