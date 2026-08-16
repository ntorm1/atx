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

function brokerEvidence(operation, canonicalSha, { cwd = 'C:\\atx', command = 'fixed broker operation', output = 'PASS', afterCanonical = canonicalSha } = {}) {
  const guard = rootGuard(canonicalSha)
  return {
    logical_operation: operation, physical_cwd: cwd, command,
    exit_code: 0, output, raw_output_sha256: '5'.repeat(64),
    root_guard_before: guard, root_guard_after: rootGuard(afterCanonical),
  }
}

const STAGE1 = {
  base: '3025895fea5f569c098090015d90b8b206e8d5a1', sha: 'a'.repeat(40), tree: 'c'.repeat(40), baseTree: 'd'.repeat(40),
  digest: '44a7b6641616161ede494a3e0353cb7ae5fb83db65b358b6c803ee915aa9f1c0', recoveryId: 'e'.repeat(64),
  sourceCommit: '58a94584baabae8263d16421f633540b420de10b', sourceTree: '6a64d8df30456b1dc4ca1e244f29a7affb77c786',
  paths: ['atx-vol/bench/oracle/bootstrap/data.json', 'atx-vol/bench/oracle/cohorts/holdout.sha256'],
  blobs: ['bb7ce65e891f8f417f4c71af0769ac84b20531fa', '66e49a2b4e8835b97e6c2c3d546f345dc751bad0'],
  gates: ['aggregate_store', 'ingest_manifest', 'cohort_manifests', 'holdout_digest'],
  commands: {
    aggregate_store: 'powershell scripts\\oracle-bootstrap-preflight.ps1 -Gate aggregate_store',
    ingest_manifest: 'powershell scripts\\oracle-bootstrap-preflight.ps1 -Gate ingest_manifest',
    cohort_manifests: 'powershell scripts\\oracle-bootstrap-preflight.ps1 -Gate cohort_manifests',
    holdout_digest: 'powershell scripts\\oracle-bootstrap-preflight.ps1 -Gate holdout_digest',
  },
}

function leaseIdentity(kind) {
  const run = `vol-oracle-${STAGE1.base}-missing_data`
  const slug = kind === 'build' ? 'data' : 'integration'
  return {
    lease_name: kind === 'build' ? 'pool-1' : 'pool-2', run_id: run,
    branch: kind === 'build' ? `lane/oracle-bootstrap-data-${run}` : `integration/oracle-bootstrap-data-${run}`,
    base_sha: STAGE1.base, worktree: kind === 'build' ? 'C:\\atx-wt\\pool-1' : 'C:\\atx-wt\\pool-2',
    heartbeat_id: `${run}-bootstrap-${slug}`, keeper_pid: kind === 'build' ? 101 : 202,
    keeper_process_started_utc: '2026-08-16T12:00:00.0000000Z', keeper_ready_utc: '2026-08-16T12:00:01.0000000Z',
  }
}

function leaseReceipt(identity, action) {
  const output = action === 'acquire'
    ? `LEASED pool=${identity.lease_name} path=${identity.worktree} branch=${identity.branch} base_sha=${identity.base_sha} run_id=${identity.run_id} owner_kind=heartbeat heartbeat_id=${identity.heartbeat_id} keeper_pid=${identity.keeper_pid} keeper_started_utc=${identity.keeper_process_started_utc} keeper_ready_utc=${identity.keeper_ready_utc}`
    : `${action.toUpperCase()} ${identity.lease_name} run_id=${identity.run_id}`
  return { action, ...identity, exit_code: 0, output }
}

function acquire(kind) {
  const identity = leaseIdentity(kind)
  return {
    capability: (kind === 'build' ? '6' : '7').repeat(64), operation_id: kind === 'build' ? 'bootstrap_data' : 'bootstrap_integration',
    stage: kind === 'build' ? 'bootstrap-1' : 'bootstrap-prepare', ...identity, lease_start_sha: STAGE1.base, recovery_replay: false,
    acquisition_receipt: leaseReceipt(identity, 'acquire'), broker_evidence: brokerEvidence('lane_open', null),
  }
}

function release(kind, sha = STAGE1.sha, tree = STAGE1.tree) {
  const identity = leaseIdentity(kind)
  return {
    released: true, lease_name: identity.lease_name, sha, tree, finalize_capability: kind === 'integration' ? '8'.repeat(64) : '',
    release_receipt: leaseReceipt(identity, 'release'), broker_evidence: brokerEvidence('lane_release', null),
  }
}

function gateReceipt(gateId, sha, tree, cwd, receiptSeed) {
  const result = { schema_version: 1, status: 'PASS', observations: 1, command_id: gateId, raw_output_sha256: '9'.repeat(64) }
  const output = JSON.stringify(result)
  const command = STAGE1.commands[gateId]
  return {
    receipt_id: receiptSeed.repeat(64), gate_id: gateId, tested_sha: sha, tested_tree: tree, command, exit_code: 0, output, result,
    broker_evidence: brokerEvidence(`gate:${gateId}`, null, { cwd, command, output }),
  }
}

function sourceReceipt() {
  return {
    source_commit: STAGE1.sourceCommit, source_parent: STAGE1.base, source_tree: STAGE1.sourceTree, adoption_rerun: false,
    blobs: STAGE1.paths.map((path, index) => ({ path, source_blob: STAGE1.blobs[index], replay_blob: STAGE1.blobs[index] })),
  }
}

function changedPathReceipt() {
  return {
    base_sha: STAGE1.base, tested_sha: STAGE1.sha, command: `git diff --name-only ${STAGE1.base}...${STAGE1.sha}`,
    exit_code: 0, output: STAGE1.paths.join('\n'), paths: STAGE1.paths,
  }
}

function stage1Report(buildAcquire, replayed = false) {
  const recoveryGates = STAGE1.gates.map((gate, index) => gateReceipt(gate, STAGE1.base, STAGE1.baseTree, buildAcquire.worktree, String(index + 1)))
  return {
    state: 'missing_data', outcome: 'DONE', branch: buildAcquire.branch, sha: STAGE1.sha, tree: STAGE1.tree, base_sha: STAGE1.base,
    worktree: buildAcquire.worktree, lease_name: buildAcquire.lease_name, lease_run_id: buildAcquire.run_id, heartbeat_id: buildAcquire.heartbeat_id,
    keeper_pid: buildAcquire.keeper_pid, keeper_process_started_utc: buildAcquire.keeper_process_started_utc, acquisition_receipt: buildAcquire.acquisition_receipt,
    holdout_digest_receipt: STAGE1.digest, bootstrap_path: 'data_recovery', recovery_id: STAGE1.recoveryId, recovery_replayed: replayed,
    recovery_source_receipt: sourceReceipt(), changed_path_receipt: changedPathReceipt(),
    evidence: recoveryGates.map(({ command, exit_code, output }) => ({ command, exit_code, output })),
    broker_evidence: [brokerEvidence(replayed ? 'recover_stage1_replay' : 'lane_commit', null, { cwd: buildAcquire.worktree })], deviations: '',
  }
}

function recoveryQuery(buildAcquire) {
  const recoveryGates = STAGE1.gates.map((gate, index) => {
    const { result: _parsed, ...raw } = gateReceipt(gate, STAGE1.base, STAGE1.baseTree, buildAcquire.worktree, String(index + 1))
    return raw
  })
  return {
    found: true, recovery_id: STAGE1.recoveryId,
    result: {
      recovery_id: STAGE1.recoveryId, replayed: true, recovery: sourceReceipt(), replay_paths: STAGE1.paths,
      gate_receipts: recoveryGates, sha: STAGE1.sha, tree: STAGE1.tree, files_changed: STAGE1.paths,
      changed_path_receipt: changedPathReceipt(), broker_evidence: [brokerEvidence('recover_stage1_replay', null, { cwd: buildAcquire.worktree })],
    },
    broker_evidence: brokerEvidence('recovery_result', null, { cwd: buildAcquire.worktree }),
  }
}

async function runStage1Scenario(mode) {
  const buildAcquire = acquire('build')
  const integrationAcquire = acquire('integration')
  const labels = []
  const phases = []
  const agent = async (_prompt, options) => {
    labels.push(options.label)
    switch (options.label) {
      case 'capability': return {
        state: 'missing_data', canonical_ref: 'refs/heads/oracle/canonical', canonical_exists: false, base_ref: 'main', base_sha: STAGE1.base,
        holdout_digest_receipt: '', next_iter: 'iter-0', evidence: [{ command: 'powershell scripts\\oracle-capability.ps1', exit_code: 0, output: 'state=missing_data\ncanonical_exists=false' }],
        broker_evidence: brokerEvidence('capability_probe', null),
      }
      case 'bootstrap-acquire:missing_data': return buildAcquire
      case 'bootstrap-build:missing_data':
        if (!['failure', 'lost'].includes(mode)) return { report: stage1Report(buildAcquire) }
        throw new Error('simulated worker response loss')
      case 'bootstrap-stage1-recovery-result':
        if (mode === 'lost') return recoveryQuery(buildAcquire)
        return { found: false, recovery_id: '', broker_evidence: brokerEvidence('recovery_result', null, { cwd: buildAcquire.worktree }) }
      case 'bootstrap-review:missing_data': return {
        verdict: 'APPROVE', reviewed_sha: STAGE1.sha, findings: [],
        evidence: [{ command: `git diff --no-ext-diff ${STAGE1.base}...${STAGE1.sha}`, exit_code: 0, output: 'reviewed' }],
        broker_evidence: [brokerEvidence('commit_inspect', null)],
      }
      case 'bootstrap-build-release': return release('build', mode === 'failure' ? STAGE1.base : STAGE1.sha, mode === 'failure' ? STAGE1.baseTree : STAGE1.tree)
      case 'bootstrap-integration-acquire:missing_data': return integrationAcquire
      case 'bootstrap-verify:missing_data': {
        const integrationReceipt = {
          reviewed_sha: STAGE1.sha, reviewed_tree: STAGE1.tree, head_after: STAGE1.sha, tree_after: STAGE1.tree,
          command: `git merge --ff-only ${STAGE1.sha}`, exit_code: 0, output: `integrated ${STAGE1.sha}`,
          broker_evidence: brokerEvidence('lane_integrate', null, { cwd: integrationAcquire.worktree }),
        }
        const gateReceipts = STAGE1.gates.map((gate, index) => gateReceipt(gate, STAGE1.sha, STAGE1.tree, integrationAcquire.worktree, String(index + 5)))
        if (mode === 'gate-omission') delete gateReceipts[0].tested_tree
        if (mode === 'gate-mismatch') gateReceipts[0].tested_sha = STAGE1.base
        if (mode === 'gate-duplicate') gateReceipts[1].receipt_id = gateReceipts[0].receipt_id
        if (mode === 'gate-freestanding') delete gateReceipts[0].broker_evidence
        const verified = {
          passed: true, reviewed_sha: STAGE1.sha, reviewed_tree: STAGE1.tree, integration_branch: integrationAcquire.branch,
          integration_sha: STAGE1.sha, integration_tree: STAGE1.tree, integration_worktree: integrationAcquire.worktree, integration_lease: integrationAcquire.lease_name,
          lease_run_id: integrationAcquire.run_id, integration_heartbeat_id: integrationAcquire.heartbeat_id, keeper_pid: integrationAcquire.keeper_pid,
          keeper_process_started_utc: integrationAcquire.keeper_process_started_utc, holdout_digest_receipt: STAGE1.digest, next_state: 'missing_mode_a',
          integration_receipt: integrationReceipt,
          head_receipt: { ref: 'HEAD', sha: STAGE1.sha, tree: STAGE1.tree, command: 'git rev-parse HEAD', exit_code: 0, output: STAGE1.sha },
          gate_receipts: gateReceipts,
          broker_evidence: [integrationReceipt.broker_evidence, ...STAGE1.gates.map((gate, index) => gateReceipt(gate, STAGE1.sha, STAGE1.tree, integrationAcquire.worktree, String(index + 5)).broker_evidence)],
        }
        if (mode === 'gate-freestanding') verified.evidence = [{ command: gateReceipts[0].command, exit_code: 0, output: gateReceipts[0].output }]
        return verified
      }
      case 'bootstrap-integration-release': return release('integration')
      case 'bootstrap-cas-finalizer': return {
        ref: 'refs/heads/oracle/canonical', new_sha: STAGE1.sha, new_tree: STAGE1.tree, expected_old_sha: '0'.repeat(40),
        command: `git update-ref refs/heads/oracle/canonical ${STAGE1.sha} ${'0'.repeat(40)}`, exit_code: 0, output: STAGE1.sha,
        broker_evidence: brokerEvidence('canonical_finalize', null, { afterCanonical: STAGE1.sha }),
      }
      case 'bootstrap-post-cas-audit': return {
        ref: 'refs/heads/oracle/canonical', sha: STAGE1.sha, command: 'git rev-parse refs/heads/oracle/canonical', exit_code: 0, output: STAGE1.sha,
        broker_evidence: brokerEvidence('canonical_audit', STAGE1.sha),
      }
      default: throw new Error(`unexpected workflow label: ${options.label}`)
    }
  }
  const result = await compileWorkflow(oracle)({}, agent, value => phases.push(value), async () => { throw new Error('nested workflow reached') }, async () => {})
  return { result, labels, phases }
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
  const resultReader = agentContract('vol-stage1-result-reader')
  assert.match(resultReader, /^tools: mcp__oracle_lane_broker__recovery_result$/m)
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

test('mocked Stage 1 happy path binds every integration gate to reviewed SHA/tree and lands once', async () => {
  const { result, labels } = await runStage1Scenario('happy')
  assert.equal(result.verdict, 'BOOTSTRAP')
  assert.equal(result.canonical_after, STAGE1.sha)
  assert.equal(labels.filter(label => label === 'bootstrap-cas-finalizer').length, 1)
  assert.equal(labels.includes('bootstrap-stage1-recovery-result'), false)
  assert.ok(result.bootstrap.verify.gate_receipts.every(receipt => receipt.tested_sha === STAGE1.sha && receipt.tested_tree === STAGE1.tree && receipt.broker_evidence))
})

test('mocked Stage 1 worker failure queries durable result before clean release and stops without integration', async () => {
  const { result, labels } = await runStage1Scenario('failure')
  assert.equal(result.verdict, 'FAILED')
  assert.match(result.failure, /sealed recovery unavailable/)
  assert.ok(labels.indexOf('bootstrap-stage1-recovery-result') < labels.indexOf('bootstrap-build-release'))
  assert.equal(labels.some(label => label.startsWith('bootstrap-integration')), false)
  assert.equal(labels.includes('bootstrap-cas-finalizer'), false)
})

test('mocked Stage 1 lost worker response resumes from sealed result and finalizes exact replay once', async () => {
  const { result, labels } = await runStage1Scenario('lost')
  assert.equal(result.verdict, 'BOOTSTRAP')
  assert.equal(result.bootstrap.report.recovery_replayed, true)
  assert.equal(result.bootstrap.report.recovery_id, STAGE1.recoveryId)
  assert.ok(labels.indexOf('bootstrap-stage1-recovery-result') < labels.indexOf('bootstrap-review:missing_data'))
  assert.equal(labels.filter(label => label === 'bootstrap-build:missing_data').length, 1)
  assert.equal(labels.filter(label => label === 'bootstrap-cas-finalizer').length, 1)
})

test('mocked Stage 1 rejects omitted, mismatched, duplicated, and free-standing gate proof', async t => {
  for (const mode of ['gate-omission', 'gate-mismatch', 'gate-duplicate', 'gate-freestanding']) {
    await t.test(mode, async () => {
      const { result, labels } = await runStage1Scenario(mode)
      assert.equal(result.verdict, 'FAILED')
      assert.match(result.failure, /gate receipt/)
      assert.equal(labels.includes('bootstrap-cas-finalizer'), false)
    })
  }
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
