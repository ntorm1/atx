import test from 'node:test'
import assert from 'node:assert/strict'
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { spawn, spawnSync } from 'node:child_process'
import { createHash, createHmac } from 'node:crypto'

import { CANONICAL_REF, GATE_REGISTRY, MAIN_REF, OPERATION_REGISTRY, OracleLaneBroker, RECOVERY_SOURCE, TOOL_DEFINITIONS } from '../oracle-lane-broker.mjs'

const projectRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..')

function run(exe, args, cwd, input) {
  const result = spawnSync(exe, args, { cwd, input, encoding: 'utf8', windowsHide: true, env: { ...process.env, GIT_CONFIG_COUNT: '1', GIT_CONFIG_KEY_0: 'safe.directory', GIT_CONFIG_VALUE_0: '*' } })
  if (result.status !== 0) throw new Error(`${exe} ${args.join(' ')} failed: ${result.stdout || ''}${result.stderr || ''}`)
  return String(result.stdout || '').trim()
}

function git(root, ...args) { return run('git', args, root) }

function write(root, rel, content) {
  const target = join(root, ...rel.split('/'))
  mkdirSync(dirname(target), { recursive: true })
  writeFileSync(target, content, 'utf8')
}

function fixture({ failGate = '', descendantBase = false, requireCommittedHead = false } = {}) {
  const sandbox = mkdtempSync(join(tmpdir(), 'atx-oracle-broker-v3-'))
  const root = join(sandbox, 'repo')
  const poolRoot = join(sandbox, 'atx-wt')
  mkdirSync(root, { recursive: true })
  git(root, 'init', '-b', 'main')
  git(root, 'config', 'user.email', 'broker-test@example.invalid')
  git(root, 'config', 'user.name', 'Oracle Broker Test')
  mkdirSync(join(root, 'scripts'), { recursive: true })
  copyFileSync(join(projectRoot, 'scripts', 'lease-worktree.ps1'), join(root, 'scripts', 'lease-worktree.ps1'))
  copyFileSync(join(projectRoot, 'scripts', 'lease-heartbeat-keeper.ps1'), join(root, 'scripts', 'lease-heartbeat-keeper.ps1'))
  write(root, '.gitignore', '/build/\n.atx-lease\n')
  write(root, 'atx-vol/CHANGELOG.md', '# changelog\n')
  write(root, 'atx-vol/src/pricing/american.cpp', 'int oracle_fixture() { return 1; }\n')
  write(root, 'atx-vol/bench/oracle/bootstrap/mode-a.json', '{"schema_version":1}\n')
  write(root, 'scripts/oracle-bootstrap-preflight.ps1', [
    'param([string]$Gate)',
    ...(requireCommittedHead ? [
      '$headData = git show HEAD:atx-vol/bench/oracle/bootstrap/data.json 2>$null',
      `if ($LASTEXITCODE -ne 0 -or -not ([string]$headData).Contains('"transition":"data"')) { [Console]::Error.Write('HEAD data unavailable'); exit 19 }`,
    ] : []),
    `if ($Gate -eq '${failGate}') { [Console]::Error.Write('forced gate failure'); exit 17 }`,
    "$value = [ordered]@{schema_version=1;status='PASS';observations=1;command_id=$Gate;raw_output_sha256=('a' * 64)}",
    '[Console]::Out.Write(($value | ConvertTo-Json -Compress))',
  ].join('\r\n'))
  git(root, 'add', '--', '.gitignore', 'scripts', 'atx-vol')
  git(root, 'commit', '-m', 'fixture base')
  const sourceParent = git(root, 'rev-parse', 'HEAD')

  git(root, 'switch', '-c', 'preserved-stage1')
  write(root, 'atx-vol/bench/oracle/bootstrap/data.json', '{"schema_version":1,"transition":"data"}\n')
  write(root, 'atx-vol/bench/oracle/cohorts/holdout.sha256', `${'4'.repeat(64)}\n`)
  git(root, 'add', '--', 'atx-vol/bench/oracle/bootstrap/data.json', 'atx-vol/bench/oracle/cohorts/holdout.sha256')
  git(root, 'commit', '-m', 'preserved stage1')
  const sourceCommit = git(root, 'rev-parse', 'HEAD')
  const sourceTree = git(root, 'show', '-s', '--format=%T', sourceCommit)
  const files = {
    'atx-vol/bench/oracle/bootstrap/data.json': git(root, 'rev-parse', `${sourceCommit}:atx-vol/bench/oracle/bootstrap/data.json`),
    'atx-vol/bench/oracle/cohorts/holdout.sha256': git(root, 'rev-parse', `${sourceCommit}:atx-vol/bench/oracle/cohorts/holdout.sha256`),
  }
  git(root, 'switch', 'main')
  if (descendantBase) {
    write(root, 'atx-vol/CHANGELOG.md', '# changelog\n\n- descendant frozen base\n')
    git(root, 'add', '--', 'atx-vol/CHANGELOG.md')
    git(root, 'commit', '-m', 'fixture descendant base')
  }
  const base = git(root, 'rev-parse', 'HEAD')
  mkdirSync(poolRoot, { recursive: true })
  const pool = join(poolRoot, 'pool-1')
  git(root, 'worktree', 'add', '--detach', pool, base)
  mkdirSync(join(pool, 'build'), { recursive: true })
  writeFileSync(join(pool, 'build', 'build.ninja'), '# warm fixture\n', 'utf8')

  const gateRegistry = { ...GATE_REGISTRY }
  for (const gateId of ['aggregate_store', 'ingest_manifest', 'cohort_manifests', 'holdout_digest', 'mode_a_targeted_tests', 'mode_a_smoke', 'convention_tests', 'mode_a_smoke_tune', 'residual_floor', 'convention_speed', 'mode_b_targeted_tests', 'mode_b_smoke_tune']) {
    gateRegistry[gateId] = { display: `powershell scripts\\oracle-bootstrap-preflight.ps1 -Gate ${gateId}`, file: 'scripts/oracle-bootstrap-preflight.ps1', args: ['-Gate', gateId] }
  }
  const broker = new OracleLaneBroker({ root, poolRoot, testMode: true, gateRegistry, recoverySource: { commit: sourceCommit, parent: sourceParent, tree: sourceTree, files } })
  return { sandbox, root, poolRoot, pool, base, sourceParent, sourceCommit, sourceTree, files, broker }
}

test('production broker surface has fixed IDs, no raw command tool, and exact recovery pin', () => {
  assert.equal(CANONICAL_REF, 'refs/heads/oracle/canonical')
  assert.equal(MAIN_REF, 'refs/heads/main')
  assert.deepEqual(Object.keys(RECOVERY_SOURCE.files).sort(), [
    'atx-vol/bench/oracle/bootstrap/data.json',
    'atx-vol/bench/oracle/cohorts/holdout.sha256',
  ])
  assert.equal(RECOVERY_SOURCE.commit, '58a94584baabae8263d16421f633540b420de10b')
  assert.ok(OPERATION_REGISTRY.bootstrap_data)
  assert.ok(OPERATION_REGISTRY.bootstrap_integration.finalize)
  assert.deepEqual(OPERATION_REGISTRY.bootstrap_conventions.prefixes, [])
  assert.deepEqual(OPERATION_REGISTRY.bootstrap_integration.prefixes, [])
  assert.ok(OPERATION_REGISTRY.bootstrap_conventions.exact.includes('atx-vol/tools/oracle_convention_sweep.cpp'))
  assert.ok(OPERATION_REGISTRY.bootstrap_integration.exact.includes('scripts/oracle-targeted-gate.ps1'))
  assert.equal(OPERATION_REGISTRY.bootstrap_conventions.exact.includes('atx-vol/docs/oracle/NORTHSTAR.md'), false)
  assert.equal(OPERATION_REGISTRY.bootstrap_conventions.exact.includes('atx-vol/docs/LEDGER.md'), false)
  assert.equal(TOOL_DEFINITIONS.some(tool => /command|shell|powershell|bash/i.test(tool.name)), false)
  for (const tool of TOOL_DEFINITIONS) {
    assert.equal(tool.inputSchema.type, 'object')
    assert.equal(tool.inputSchema.additionalProperties, false)
    assert.equal(Object.hasOwn(tool.inputSchema.properties || {}, 'cwd'), false)
    assert.equal(Object.hasOwn(tool.inputSchema.properties || {}, 'path'), false)
    assert.equal(Object.hasOwn(tool.inputSchema.properties || {}, 'command'), false)
  }

  const forbidden = 'disallowedTools: Bash, PowerShell, Edit, Write, NotebookEdit, EnterWorktree'
  const exactAgentTools = {
    'vol-lane-opener': 'tools: mcp__oracle_lane_broker__lane_open',
    'vol-lane-releaser': 'tools: mcp__oracle_lane_broker__lane_release',
    'vol-stage1-quarantiner': 'tools: mcp__oracle_lane_broker__lane_quarantine',
    'vol-stage1-recovery': 'tools: mcp__oracle_lane_broker__recover_stage1',
    'vol-stage1-result-reader': 'tools: mcp__oracle_lane_broker__recovery_result',
    'vol-builder': 'tools: mcp__oracle_lane_broker__repo_search, mcp__oracle_lane_broker__repo_read, mcp__oracle_lane_broker__workspace_list, mcp__oracle_lane_broker__patch_apply, mcp__oracle_lane_broker__gate_run, mcp__oracle_lane_broker__lane_commit',
    'vol-verifier': 'tools: mcp__oracle_lane_broker__lane_integrate, mcp__oracle_lane_broker__gate_run',
    'vol-reviewer': 'tools: mcp__oracle_lane_broker__repo_search, mcp__oracle_lane_broker__repo_read, mcp__oracle_lane_broker__commit_inspect, mcp__oracle_lane_broker__gate_run',
    'vol-ref-finalizer': 'tools: mcp__oracle_lane_broker__canonical_finalize',
    'vol-ref-auditor': 'tools: mcp__oracle_lane_broker__ref_resolve, mcp__oracle_lane_broker__canonical_audit',
    'vol-capability-inspector': 'tools: mcp__oracle_lane_broker__capability_probe',
    'vol-planner': 'tools: mcp__oracle_lane_broker__repo_search, mcp__oracle_lane_broker__repo_read',
  }
  for (const [agentName, tools] of Object.entries(exactAgentTools)) {
    const source = readFileSync(join(projectRoot, '.claude', 'agents', `${agentName}.md`), 'utf8')
    assert.ok(source.split(/\r?\n/).includes(tools))
    assert.ok(source.split(/\r?\n/).includes(forbidden))
    assert.match(source, /^permissionMode: dontAsk$/m)
  }

  const oracleWorkflow = readFileSync(join(projectRoot, '.claude', 'workflows', 'vol-oracle-iter.js'), 'utf8')
  assert.ok(oracleWorkflow.indexOf("failure: 'READY_BROKER_MIGRATION_REQUIRED'") < oracleWorkflow.indexOf("phase('Measure')"))
  for (const agentName of ['vol-lane-opener', 'vol-lane-releaser', 'vol-stage1-quarantiner', 'vol-stage1-result-reader', 'vol-reviewer', 'vol-verifier', 'vol-ref-finalizer', 'vol-ref-auditor']) {
    assert.match(oracleWorkflow, new RegExp(`agentType: '${agentName}'`))
  }
  assert.match(oracleWorkflow, /buildAgentType = capability\.state === 'missing_data' \? 'vol-stage1-recovery' : 'vol-builder'/)
  const sprintWorkflow = readFileSync(join(projectRoot, '.claude', 'workflows', 'vol-sprint.js'), 'utf8')
  assert.match(sprintWorkflow, /failure: 'ORACLE_BROKER_MIGRATION_REQUIRED'/)
  assert.doesNotMatch(sprintWorkflow, /\bagent\s*\(/)
})

test('Stage 2 clean release reopens with fresh capability and immutable history while drift rejects', { timeout: 120_000 }, async () => {
  const fx = fixture()
  const identity = { operation_id: 'bootstrap_mode_a', stage: 'bootstrap-2', run_id: 'run-stage2-reopen', branch: 'lane/oracle-bootstrap-mode-a-run-stage2-reopen', base_sha: fx.base, heartbeat_id: 'run-stage2-reopen-mode-a' }
  try {
    git(fx.root, 'update-ref', 'refs/heads/oracle/canonical', fx.base)
    const acquired = fx.broker.openLane(identity)
    assert.equal(acquired.recovery_replay, false)
    assert.equal(acquired.lease_start_sha, fx.base)
    const gitDir = git(fx.root, 'rev-parse', '--absolute-git-dir')
    const capDir = join(gitDir, 'oracle-lane-broker-v3', 'capabilities')
    const recoveryDir = join(gitDir, 'oracle-lane-broker-v3', 'recoveries')
    const firstCapPath = join(capDir, `${acquired.capability}.json`)
    let firstRecord = JSON.parse(readFileSync(firstCapPath, 'utf8'))
    assert.equal(firstRecord.attempt_epoch, 1)
    assert.deepEqual(firstRecord.reopen_predecessors, [])
    assert.equal(firstRecord.acquisition_root_guard_before.raw_sha256, firstRecord.acquisition_root_guard_after.raw_sha256)
    const changelogPath = join(fx.pool, 'atx-vol', 'CHANGELOG.md')
    const cleanChangelog = readFileSync(changelogPath, 'utf8')
    writeFileSync(changelogPath, `${cleanChangelog}\nDIRTY\n`, 'utf8')
    assert.throws(() => fx.broker.releaseLane({ capability: acquired.capability }), /refusing to release a dirty broker lane/)
    writeFileSync(changelogPath, cleanChangelog, 'utf8')
    assert.equal(fx.broker.releaseLane({ capability: acquired.capability }).sha, fx.base)
    const immutableFirstRelease = readFileSync(firstCapPath, 'utf8')
    firstRecord = JSON.parse(immutableFirstRelease)
    assert.equal(firstRecord.state, 'released')
    assert.equal(firstRecord.released_head, fx.base)
    assert.equal(firstRecord.released_tree, git(fx.root, 'show', '-s', '--format=%T', fx.base))
    assert.deepEqual(readdirSync(recoveryDir).filter(name => name.endsWith('.json')), [])

    const strandedUntracked = join(fx.pool, 'atx-vol', 'STAGE2-DIRTY.tmp')
    writeFileSync(changelogPath, `${cleanChangelog}\nDIRTY AFTER RELEASE\n`, 'utf8')
    writeFileSync(strandedUntracked, 'preserve me\n', 'utf8')
    assert.throws(() => fx.broker.openLane(identity), /post-acquisition base\/clean audit; dirty lane preserved and keeper stopped/)
    assert.match(readFileSync(changelogPath, 'utf8'), /DIRTY AFTER RELEASE/)
    assert.equal(readFileSync(strandedUntracked, 'utf8'), 'preserve me\n')
    assert.equal(readFileSync(firstCapPath, 'utf8'), immutableFirstRelease)
    assert.equal(readdirSync(capDir).filter(name => name.endsWith('.json')).length, 1)
    const strandedLeasePath = join(fx.pool, '.atx-lease')
    const strandedLease = readFileSync(strandedLeasePath, 'ascii')
    const strandedKeeperPid = Number(strandedLease.match(/^keeper_pid=(\d+)$/m)?.[1])
    assert.ok(strandedKeeperPid > 0)
    const keeperStopped = spawnSync('powershell', ['-NoProfile', '-Command', `if (Get-Process -Id ${strandedKeeperPid} -ErrorAction SilentlyContinue) { exit 1 } else { exit 0 }`], { windowsHide: true })
    assert.equal(keeperStopped.status, 0)
    writeFileSync(changelogPath, cleanChangelog, 'utf8')
    rmSync(strandedUntracked)
    run('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', join(fx.root, 'scripts', 'lease-worktree.ps1'), '-Release', 'pool-1', '-RunId', identity.run_id], fx.root)
    assert.equal(existsSync(strandedLeasePath), false)

    const reopened = fx.broker.openLane(identity)
    assert.notEqual(reopened.capability, acquired.capability)
    assert.equal(reopened.recovery_replay, false)
    assert.equal(reopened.lease_start_sha, fx.base)
    assert.equal(readFileSync(firstCapPath, 'utf8'), immutableFirstRelease)
    const secondCapPath = join(capDir, `${reopened.capability}.json`)
    const secondRecord = JSON.parse(readFileSync(secondCapPath, 'utf8'))
    assert.equal(secondRecord.state, 'active')
    assert.equal(secondRecord.attempt_epoch, 2)
    assert.deepEqual(secondRecord.reopen_predecessors, [{
      capability_hash: createHash('sha256').update(acquired.capability).digest('hex'),
      released_head: fx.base, released_tree: firstRecord.released_tree, lease_name: acquired.lease_name,
    }])
    assert.equal(secondRecord.acquisition_root_guard_before.raw_sha256, secondRecord.acquisition_root_guard_after.raw_sha256)
    assert.equal(fx.broker.releaseLane({ capability: reopened.capability }).sha, fx.base)
    assert.equal(readFileSync(firstCapPath, 'utf8'), immutableFirstRelease)
    assert.equal(git(fx.root, 'rev-parse', `refs/heads/${identity.branch}`), fx.base)
    assert.deepEqual(git(fx.pool, 'status', '--porcelain=v1', '-uall').split(/\r?\n/).filter(Boolean), [])

    assert.throws(() => fx.broker.openLane({ ...identity, heartbeat_id: 'run-stage2-reopen-wrong' }), /identity conflicts/)
    assert.throws(() => fx.broker.openLane({ ...identity, base_sha: fx.sourceCommit }), /identity conflicts/)

    git(fx.root, 'update-ref', `refs/heads/${identity.branch}`, fx.sourceCommit, fx.base)
    assert.throws(() => fx.broker.openLane(identity), /clean-base reopen audit/)
    git(fx.root, 'update-ref', `refs/heads/${identity.branch}`, fx.base, fx.sourceCommit)

    const quarantinePath = join(fx.pool, '.atx-quarantine-v3')
    writeFileSync(quarantinePath, `version=3\nrun_id=${identity.run_id}\nbranch=${identity.branch}\nheartbeat_id=${identity.heartbeat_id}\n`, 'ascii')
    assert.throws(() => fx.broker.openLane(identity), /quarantined for audit/)
    rmSync(quarantinePath)

    const secret = readFileSync(join(gitDir, 'oracle-lane-broker-v3', 'secret'), 'ascii').trim()
    const { seal: _seal, ...wrongTreeBody } = JSON.parse(immutableFirstRelease)
    wrongTreeBody.released_tree = '0'.repeat(40)
    const wrongTreeSeal = createHmac('sha256', secret).update(JSON.stringify(wrongTreeBody)).digest('hex')
    writeFileSync(firstCapPath, JSON.stringify({ ...wrongTreeBody, seal: wrongTreeSeal }), 'utf8')
    assert.throws(() => fx.broker.openLane(identity), /clean-base reopen audit/)
    writeFileSync(firstCapPath, immutableFirstRelease, 'utf8')

    writeFileSync(firstCapPath, immutableFirstRelease.replace('"state":"released"', '"state":"tampered"'), 'utf8')
    assert.throws(() => fx.broker.openLane(identity), /capability seal mismatch/)
    writeFileSync(firstCapPath, immutableFirstRelease, 'utf8')
    assert.deepEqual(readdirSync(recoveryDir).filter(name => name.endsWith('.json')), [])
  } finally {
    try { run('git', ['worktree', 'remove', '--force', fx.pool], fx.root) } catch { }
    await new Promise(resolveDelay => setTimeout(resolveDelay, 250))
    try {
      rmSync(fx.sandbox, { recursive: true, force: true, maxRetries: 8, retryDelay: 100 })
    } catch (error) {
      if (error?.code !== 'EPERM') throw error
      const cleanup = spawn(process.execPath, ['-e', `setTimeout(()=>require('fs').rmSync(${JSON.stringify(fx.sandbox)},{recursive:true,force:true,maxRetries:20,retryDelay:200}),1000)`], { detached: true, stdio: 'ignore', windowsHide: true })
      cleanup.unref()
    }
  }
})

test('Stage 1 recovery commits atop a current descendant base and reopens only by sealed replay', { timeout: 120_000 }, async () => {
  const fx = fixture({ descendantBase: true, requireCommittedHead: true })
  const identity = { operation_id: 'bootstrap_data', stage: 'bootstrap-1', run_id: 'run-descendant', branch: 'lane/oracle-bootstrap-data-run-descendant', base_sha: fx.base, heartbeat_id: 'run-descendant-data' }
  try {
    assert.notEqual(fx.base, fx.sourceParent)
    git(fx.root, 'merge-base', '--is-ancestor', fx.sourceParent, fx.base)
    const acquire = fx.broker.openLane(identity)
    assert.equal(acquire.recovery_replay, false)
    assert.equal(acquire.lease_start_sha, fx.base)
    const recovered = fx.broker.recoverStage1({ capability: acquire.capability })
    assert.equal(recovered.replayed, false)
    assert.equal(recovered.recovery.source_parent, fx.sourceParent)
    assert.equal(git(fx.root, 'show', '-s', '--format=%P', recovered.sha), fx.base)
    assert.deepEqual(recovered.files_changed.sort(), Object.keys(fx.files).sort())
    assert.ok(recovered.gate_receipts.every(receipt => receipt.exit_code === 0 && receipt.tested_sha === recovered.sha && receipt.tested_tree === recovered.tree))
    const receiptIds = recovered.gate_receipts.map(receipt => receipt.receipt_id)
    const released = fx.broker.releaseLane({ capability: acquire.capability })
    assert.equal(released.sha, recovered.sha)

    const replayAcquire = fx.broker.openLane(identity)
    assert.equal(replayAcquire.recovery_replay, true)
    assert.equal(replayAcquire.lease_start_sha, recovered.sha)
    const replayed = fx.broker.recoverStage1({ capability: replayAcquire.capability })
    assert.equal(replayed.replayed, true)
    assert.equal(replayed.sha, recovered.sha)
    assert.deepEqual(replayed.gate_receipts.map(receipt => receipt.receipt_id), receiptIds)
    assert.equal(fx.broker.releaseLane({ capability: replayAcquire.capability }).sha, recovered.sha)
  } finally {
    try { run('git', ['worktree', 'remove', '--force', fx.pool], fx.root) } catch { }
    await new Promise(resolveDelay => setTimeout(resolveDelay, 250))
    try {
      rmSync(fx.sandbox, { recursive: true, force: true, maxRetries: 8, retryDelay: 100 })
    } catch (error) {
      if (error?.code !== 'EPERM') throw error
      const cleanup = spawn(process.execPath, ['-e', `setTimeout(()=>require('fs').rmSync(${JSON.stringify(fx.sandbox)},{recursive:true,force:true,maxRetries:20,retryDelay:200}),1000)`], { detached: true, stdio: 'ignore', windowsHide: true })
      cleanup.unref()
    }
  }
})

test('existing canonical bootstrap finalizes one exact Stage 2 descendant with sealed gates and rejects drift or substitution', { timeout: 180_000 }, async () => {
  const fx = fixture()
  const runId = 'run-existing-canonical'
  const stage2Identity = { operation_id: 'bootstrap_mode_a', stage: 'bootstrap-2', run_id: runId, branch: 'lane/oracle-bootstrap-mode-a-run-existing-canonical', base_sha: fx.base, heartbeat_id: 'run-existing-canonical-mode-a' }
  try {
    git(fx.root, 'update-ref', 'refs/heads/oracle/canonical', fx.base)
    const stage2 = fx.broker.openLane(stage2Identity)
    const stage2Patch = 'diff --git a/atx-vol/bench/oracle/bootstrap/mode-a.json b/atx-vol/bench/oracle/bootstrap/mode-a.json\n--- a/atx-vol/bench/oracle/bootstrap/mode-a.json\n+++ b/atx-vol/bench/oracle/bootstrap/mode-a.json\n@@ -1 +1 @@\n-{"schema_version":1}\n+{"schema_version":1,"rows_processed":1}\n'
    fx.broker.applyPatch({ capability: stage2.capability, patch: stage2Patch })
    const candidate = fx.broker.commitLane({ capability: stage2.capability, message_id: 'bootstrap_mode_a' })
    assert.match(candidate.file_blob_oids['atx-vol/bench/oracle/bootstrap/mode-a.json'], /^[0-9a-f]{40}$/)
    assert.equal(fx.broker.releaseLane({ capability: stage2.capability }).finalize_capability, '')

    const wrongOperation = fx.broker.openLane({ operation_id: 'sprint_build', stage: 'improve', run_id: runId, branch: 'lane/wrong-operation-run-existing-canonical', base_sha: fx.base, heartbeat_id: 'run-existing-canonical-wrong-operation' })
    const wrongOperationPatch = 'diff --git a/atx-vol/CHANGELOG.md b/atx-vol/CHANGELOG.md\n--- a/atx-vol/CHANGELOG.md\n+++ b/atx-vol/CHANGELOG.md\n@@ -1 +1,2 @@\n # changelog\n+wrong operation\n'
    fx.broker.applyPatch({ capability: wrongOperation.capability, patch: wrongOperationPatch })
    const wrongOperationCommit = fx.broker.commitLane({ capability: wrongOperation.capability, message_id: 'sprint_lane' })
    fx.broker.releaseLane({ capability: wrongOperation.capability })

    const integration = fx.broker.openLane({ operation_id: 'bootstrap_integration', stage: 'bootstrap-prepare', run_id: runId, branch: 'integration/oracle-bootstrap-mode-a-run-existing-canonical', base_sha: fx.base, heartbeat_id: 'run-existing-canonical-integration' })
    git(fx.root, 'update-ref', 'refs/heads/oracle/canonical', fx.sourceCommit, fx.base)
    assert.throws(() => fx.broker.integrate({ capability: integration.capability, reviewed_shas: [candidate.sha] }), /canonical\/base precondition drifted/)
    git(fx.root, 'update-ref', 'refs/heads/oracle/canonical', fx.base, fx.sourceCommit)
    assert.throws(() => fx.broker.integrate({ capability: integration.capability, reviewed_shas: [wrongOperationCommit.sha] }), /wrong operation\/stage/)
    assert.throws(() => fx.broker.integrate({ capability: integration.capability, reviewed_shas: [fx.sourceCommit] }), /lacks an exact clean released bootstrap candidate/)

    const gitDir = git(fx.root, 'rev-parse', '--absolute-git-dir')
    const stage2CapPath = join(gitDir, 'oracle-lane-broker-v3', 'capabilities', `${stage2.capability}.json`)
    const stage2CapRaw = readFileSync(stage2CapPath, 'utf8')
    const { seal: _stage2Seal, ...stage2CapBody } = JSON.parse(stage2CapRaw)
    const secret = readFileSync(join(gitDir, 'oracle-lane-broker-v3', 'secret'), 'ascii').trim()
    const writeSealedStage2 = body => writeFileSync(stage2CapPath, JSON.stringify({ ...body, seal: createHmac('sha256', secret).update(JSON.stringify(body)).digest('hex') }), 'utf8')
    writeSealedStage2({ ...stage2CapBody, stage: 'bootstrap-3' })
    assert.throws(() => fx.broker.integrate({ capability: integration.capability, reviewed_shas: [candidate.sha] }), /wrong operation\/stage/)
    writeSealedStage2({ ...stage2CapBody, released_tree: '0'.repeat(40) })
    assert.throws(() => fx.broker.integrate({ capability: integration.capability, reviewed_shas: [candidate.sha] }), /lacks an exact clean released bootstrap candidate/)
    writeFileSync(stage2CapPath, stage2CapRaw, 'utf8')

    const integrated = fx.broker.integrate({ capability: integration.capability, reviewed_shas: [candidate.sha] })
    assert.equal(integrated.sha, candidate.sha)
    assert.equal(integrated.tree, candidate.tree)
    assert.throws(() => fx.broker.runGate({ capability: integration.capability, gate_id: 'convention_tests' }), /not required for the reviewed bootstrap operation\/stage/)
    assert.throws(() => fx.broker.releaseLane({ capability: integration.capability }), /gate receipt set is incomplete/)
    const gates = ['mode_a_targeted_tests', 'mode_a_smoke'].map(gate_id => fx.broker.runGate({ capability: integration.capability, gate_id }))
    assert.ok(gates.every(receipt => receipt.exit_code === 0 && receipt.tested_sha === candidate.sha && receipt.tested_tree === candidate.tree && /^[0-9a-f]{64}$/.test(receipt.receipt_id)))
    assert.throws(() => fx.broker.runGate({ capability: integration.capability, gate_id: 'mode_a_smoke' }), /already exists/)

    git(fx.root, 'update-ref', 'refs/heads/oracle/canonical', fx.sourceCommit, fx.base)
    assert.throws(() => fx.broker.releaseLane({ capability: integration.capability }), /canonical\/base precondition drifted/)
    git(fx.root, 'update-ref', 'refs/heads/oracle/canonical', fx.base, fx.sourceCommit)
    const released = fx.broker.releaseLane({ capability: integration.capability })
    assert.equal(released.sha, candidate.sha)
    assert.equal(released.tree, candidate.tree)
    assert.match(released.finalize_capability, /^[0-9a-f]{64}$/)

    assert.throws(() => fx.broker.canonicalFinalize({ finalize_capability: released.finalize_capability, expected_sha: candidate.sha, expected_tree: fx.sourceTree }), /expected SHA\/tree/)
    git(fx.root, 'update-ref', 'refs/heads/oracle/canonical', fx.sourceCommit, fx.base)
    assert.throws(() => fx.broker.canonicalFinalize({ finalize_capability: released.finalize_capability, expected_sha: candidate.sha, expected_tree: candidate.tree }), /compare-and-swap precondition is stale/)
    git(fx.root, 'update-ref', 'refs/heads/oracle/canonical', fx.base, fx.sourceCommit)
    const finalized = fx.broker.canonicalFinalize({ finalize_capability: released.finalize_capability, expected_sha: candidate.sha, expected_tree: candidate.tree })
    assert.equal(finalized.expected_old_sha, fx.base)
    assert.equal(finalized.new_sha, candidate.sha)
    assert.equal(finalized.new_tree, candidate.tree)
    assert.equal(git(fx.root, 'rev-parse', 'refs/heads/main'), fx.base)
    assert.equal(git(fx.root, 'rev-parse', 'refs/heads/oracle/canonical'), candidate.sha)
    assert.throws(() => fx.broker.canonicalFinalize({ finalize_capability: released.finalize_capability, expected_sha: candidate.sha, expected_tree: candidate.tree }), /unknown|stale|consumed/)
    assert.throws(() => fx.broker.openLane({ operation_id: 'bootstrap_integration', stage: 'bootstrap-prepare', run_id: 'run-main-substitution', branch: 'integration/oracle-bootstrap-main-substitution', base_sha: fx.base, heartbeat_id: 'run-main-substitution-integration' }), /base must equal the current canonical ref/)
  } finally {
    try { run('git', ['worktree', 'remove', '--force', fx.pool], fx.root) } catch { }
    await new Promise(resolveDelay => setTimeout(resolveDelay, 250))
    try {
      rmSync(fx.sandbox, { recursive: true, force: true, maxRetries: 8, retryDelay: 100 })
    } catch (error) {
      if (error?.code !== 'EPERM') throw error
      const cleanup = spawn(process.execPath, ['-e', `setTimeout(()=>require('fs').rmSync(${JSON.stringify(fx.sandbox)},{recursive:true,force:true,maxRetries:20,retryDelay:200}),1000)`], { detached: true, stdio: 'ignore', windowsHide: true })
      cleanup.unref()
    }
  }
})

test('temp repo broker rejects incident classes and proves recovery, lane commit/release, integration, and canonical CAS', { timeout: 240_000 }, async () => {
  const fx = fixture()
  try {
    const rootBefore = fx.broker.rootGuard()
    assert.equal(rootBefore.main_sha, fx.base)
    assert.equal(rootBefore.canonical_sha, null)
    assert.throws(() => fx.broker.openLane({ operation_id: 'bootstrap_data', stage: 'wrong', run_id: 'run-stage1', branch: 'lane/oracle-bootstrap-data-run-stage1', base_sha: fx.base, heartbeat_id: 'run-stage1-data' }), /operation\/stage/)

    const acquire = fx.broker.openLane({ operation_id: 'bootstrap_data', stage: 'bootstrap-1', run_id: 'run-stage1', branch: 'lane/oracle-bootstrap-data-run-stage1', base_sha: fx.base, heartbeat_id: 'run-stage1-data' })
    assert.match(acquire.capability, /^[0-9a-f]{64}$/)
    assert.equal(acquire.worktree, fx.pool)
    assert.equal(acquire.broker_evidence.physical_cwd, fx.root)
    assert.equal(acquire.broker_evidence.root_guard_before.raw_sha256, acquire.broker_evidence.root_guard_after.raw_sha256)
    const idempotent = fx.broker.openLane({ operation_id: 'bootstrap_data', stage: 'bootstrap-1', run_id: 'run-stage1', branch: 'lane/oracle-bootstrap-data-run-stage1', base_sha: fx.base, heartbeat_id: 'run-stage1-data' })
    assert.equal(idempotent.capability, acquire.capability)

    const escapePatch = 'diff --git a/../repo/pwned.txt b/../repo/pwned.txt\nnew file mode 100644\n--- /dev/null\n+++ b/../repo/pwned.txt\n@@ -0,0 +1 @@\n+pwned\n'
    assert.throws(() => fx.broker.applyPatch({ capability: acquire.capability, patch: escapePatch }), /forbidden for bootstrap_data/)
    assert.throws(() => fx.broker.commitLane({ capability: acquire.capability, message_id: 'bootstrap_data_recovery' }), /forbidden for bootstrap_data/)
    assert.throws(() => fx.broker.runGate({ capability: acquire.capability, gate_id: 'aggregate_store' }), /use recover_stage1/)

    const leasePath = join(fx.pool, '.atx-lease')
    const lease = readFileSync(leasePath, 'ascii')
    writeFileSync(leasePath, lease.replace(/keeper_pid=\d+/, 'keeper_pid=99999999'), 'ascii')
    assert.throws(() => fx.broker.listWorkspace({ capability: acquire.capability }), /lease mismatch/)
    writeFileSync(leasePath, lease, 'ascii')

    // Simulate a transport loss: the successful mutation result is deliberately
    // ignored.  Cleanup may still release the now-clean lane.
    fx.broker.recoverStage1({ capability: acquire.capability })
    const released = fx.broker.releaseLane({ capability: acquire.capability })
    assert.equal(released.finalize_capability, '')
    assert.throws(() => fx.broker.listWorkspace({ capability: acquire.capability }), /stale|inactive/)

    const gitDir = git(fx.root, 'rev-parse', '--absolute-git-dir')
    const recoveryDir = join(gitDir, 'oracle-lane-broker-v3', 'recoveries')
    const recoveryFiles = readdirSync(recoveryDir).filter(name => name.endsWith('.json'))
    assert.equal(recoveryFiles.length, 1)
    const recoveryPath = join(recoveryDir, recoveryFiles[0])
    const sealedRecovery = readFileSync(recoveryPath, 'utf8')

    writeFileSync(recoveryPath, sealedRecovery.replace('"state":"sealed"', '"state":"broken"'), 'utf8')
    assert.throws(() => fx.broker.openLane({ operation_id: 'bootstrap_data', stage: 'bootstrap-1', run_id: 'run-stage1', branch: 'lane/oracle-bootstrap-data-run-stage1', base_sha: fx.base, heartbeat_id: 'run-stage1-data' }), /seal mismatch/)
    writeFileSync(recoveryPath, sealedRecovery, 'utf8')

    const wrongIdentity = { version: 3, operation_id: 'bootstrap_data', stage: 'bootstrap-1', run_id: 'run-wrong', branch: 'lane/oracle-bootstrap-data-run-wrong', base_sha: fx.base, heartbeat_id: 'run-wrong-data', source_commit: fx.sourceCommit }
    const wrongPath = join(recoveryDir, `${createHash('sha256').update(Buffer.from(JSON.stringify(wrongIdentity), 'utf8')).digest('hex')}.json`)
    writeFileSync(wrongPath, sealedRecovery, 'utf8')
    assert.throws(() => fx.broker.openLane({ operation_id: 'bootstrap_data', stage: 'bootstrap-1', run_id: 'run-wrong', branch: 'lane/oracle-bootstrap-data-run-wrong', base_sha: fx.base, heartbeat_id: 'run-wrong-data' }), /identity mismatch/)
    rmSync(wrongPath)

    git(fx.root, 'update-ref', 'refs/heads/lane/oracle-bootstrap-data-run-stage1', fx.sourceCommit, released.sha)
    assert.throws(() => fx.broker.openLane({ operation_id: 'bootstrap_data', stage: 'bootstrap-1', run_id: 'run-stage1', branch: 'lane/oracle-bootstrap-data-run-stage1', base_sha: fx.base, heartbeat_id: 'run-stage1-data' }), /branch is missing or ahead/)
    git(fx.root, 'update-ref', 'refs/heads/lane/oracle-bootstrap-data-run-stage1', released.sha, fx.sourceCommit)

    rmSync(recoveryPath)
    assert.throws(() => fx.broker.openLane({ operation_id: 'bootstrap_data', stage: 'bootstrap-1', run_id: 'run-stage1', branch: 'lane/oracle-bootstrap-data-run-stage1', base_sha: fx.base, heartbeat_id: 'run-stage1-data' }), /incompatible released/)
    writeFileSync(recoveryPath, sealedRecovery, 'utf8')

    const replayAcquire = fx.broker.openLane({ operation_id: 'bootstrap_data', stage: 'bootstrap-1', run_id: 'run-stage1', branch: 'lane/oracle-bootstrap-data-run-stage1', base_sha: fx.base, heartbeat_id: 'run-stage1-data' })
    assert.equal(replayAcquire.recovery_replay, true)
    assert.equal(replayAcquire.lease_start_sha, released.sha)
    const queried = fx.broker.recoveryResult({ capability: replayAcquire.capability })
    assert.equal(queried.found, true)
    assert.equal(queried.recovery_id, recoveryFiles[0].replace(/\.json$/, ''))
    const recovered = queried.result
    assert.equal(recovered.replayed, true)
    assert.equal(recovered.sha, released.sha)
    assert.equal(recovered.tree, released.tree)
    assert.equal(recovered.recovery.source_commit, fx.sourceCommit)
    assert.equal(recovered.recovery.source_parent, fx.base)
    assert.equal(recovered.recovery.source_tree, fx.sourceTree)
    assert.equal(recovered.recovery.adoption_rerun, false)
    assert.deepEqual(recovered.files_changed.sort(), Object.keys(fx.files).sort())
    assert.deepEqual(recovered.gate_receipts.map(item => item.gate_id), ['aggregate_store', 'ingest_manifest', 'cohort_manifests', 'holdout_digest'])
    assert.ok(recovered.gate_receipts.every(item => /^[0-9a-f]{64}$/.test(item.receipt_id) && item.tested_sha === recovered.sha && item.tested_tree === recovered.tree && item.exit_code === 0 && item.broker_evidence.physical_cwd === fx.pool))
    const replayAgain = fx.broker.recoverStage1({ capability: replayAcquire.capability })
    assert.equal(replayAgain.replayed, true)
    assert.deepEqual(replayAgain.gate_receipts.map(item => item.receipt_id), recovered.gate_receipts.map(item => item.receipt_id))

    const inspection = fx.broker.inspectCommit({ base_sha: fx.base, candidate_sha: recovered.sha })
    assert.deepEqual(inspection.paths.sort(), Object.keys(fx.files).sort())
    assert.equal(inspection.broker_evidence.root_guard_before.main_sha, fx.base)
    const replayReleased = fx.broker.releaseLane({ capability: replayAcquire.capability })
    assert.equal(replayReleased.sha, recovered.sha)
    assert.equal(replayReleased.tree, recovered.tree)

    const edit = fx.broker.openLane({ operation_id: 'bootstrap_mode_a', stage: 'bootstrap-2', run_id: 'run-mode-a', branch: 'lane/oracle-bootstrap-mode-a-run-mode-a', base_sha: fx.base, heartbeat_id: 'run-mode-a-edit' })
    const buildNinjaBefore = readFileSync(join(edit.worktree, 'build', 'build.ninja'), 'utf8')
    const hiddenTargetPatch = 'diff --git a/atx-vol/src/pricing/american.cpp b/atx-vol/src/pricing/american.cpp\n--- a/atx-vol/src/pricing/american.cpp\n+++ b/build/build.ninja\n@@ -1 +1 @@\n-# warm fixture\n+PWNED\n'
    assert.throws(() => fx.broker.applyPatch({ capability: edit.capability, patch: hiddenTargetPatch }), /path differs from diff header/)
    assert.equal(readFileSync(join(edit.worktree, 'build', 'build.ninja'), 'utf8'), buildNinjaBefore)
    const safePatch = 'diff --git a/atx-vol/src/pricing/american.cpp b/atx-vol/src/pricing/american.cpp\n--- a/atx-vol/src/pricing/american.cpp\n+++ b/atx-vol/src/pricing/american.cpp\n@@ -1 +1 @@\n-int oracle_fixture() { return 1; }\n+int oracle_fixture() { return 2; }\n'
    assert.deepEqual(fx.broker.applyPatch({ capability: edit.capability, patch: safePatch }).changed_paths, ['atx-vol/src/pricing/american.cpp'])
    const editCommit = fx.broker.commitLane({ capability: edit.capability, message_id: 'bootstrap_mode_a' })
    assert.match(editCommit.tree, /^[0-9a-f]{40}$/)
    assert.equal(fx.broker.releaseLane({ capability: edit.capability }).tree, editCommit.tree)

    const integration = fx.broker.openLane({ operation_id: 'bootstrap_integration', stage: 'bootstrap-prepare', run_id: 'run-stage1', branch: 'integration/oracle-bootstrap-data-run-stage1', base_sha: fx.base, heartbeat_id: 'run-stage1-integration' })
    assert.throws(() => fx.broker.applyPatch({ capability: integration.capability, patch: safePatch }), /forbidden for bootstrap_integration/)
    assert.throws(() => fx.broker.commitLane({ capability: integration.capability, message_id: 'bootstrap_mode_a' }), /forbidden for bootstrap_integration/)
    const integrated = fx.broker.integrate({ capability: integration.capability, reviewed_shas: [recovered.sha] })
    assert.equal(integrated.sha, recovered.sha)
    assert.equal(integrated.tree, recovered.tree)
    assert.equal(integrated.head_receipt.sha, recovered.sha)
    assert.throws(() => fx.broker.integrate({ capability: integration.capability, reviewed_shas: [recovered.sha] }), /already sealed/)
    for (const gateId of ['aggregate_store', 'ingest_manifest', 'cohort_manifests', 'holdout_digest']) assert.equal(fx.broker.runGate({ capability: integration.capability, gate_id: gateId }).exit_code, 0)
    const integrationRelease = fx.broker.releaseLane({ capability: integration.capability })
    assert.match(integrationRelease.finalize_capability, /^[0-9a-f]{64}$/)
    assert.equal(integrationRelease.sha, recovered.sha)
    assert.equal(integrationRelease.tree, recovered.tree)

    assert.throws(() => fx.broker.canonicalFinalize({ finalize_capability: integrationRelease.finalize_capability, expected_sha: fx.base, expected_tree: recovered.tree }), /expected SHA\/tree/)
    assert.throws(() => fx.broker.canonicalFinalize({ finalize_capability: integrationRelease.finalize_capability, expected_sha: recovered.sha, expected_tree: '0'.repeat(40) }), /expected SHA\/tree/)
    const finalized = fx.broker.canonicalFinalize({ finalize_capability: integrationRelease.finalize_capability, expected_sha: recovered.sha, expected_tree: recovered.tree })
    assert.equal(finalized.ref, CANONICAL_REF)
    assert.equal(finalized.new_sha, recovered.sha)
    assert.equal(finalized.new_tree, recovered.tree)
    assert.equal(finalized.expected_old_sha, '0'.repeat(40))
    assert.equal(finalized.broker_evidence.root_guard_before.main_sha, finalized.broker_evidence.root_guard_after.main_sha)
    assert.notEqual(finalized.broker_evidence.root_guard_before.canonical_sha, finalized.broker_evidence.root_guard_after.canonical_sha)
    assert.throws(() => fx.broker.canonicalFinalize({ finalize_capability: integrationRelease.finalize_capability, expected_sha: recovered.sha, expected_tree: recovered.tree }), /unknown|stale|consumed/)
    assert.equal(fx.broker.canonicalAudit().sha, recovered.sha)

    const rootAfter = fx.broker.rootGuard()
    assert.equal(rootAfter.main_sha, fx.base)
    assert.equal(rootAfter.index_sha256, rootBefore.index_sha256)
    assert.equal(rootAfter.tracked_sha256, rootBefore.tracked_sha256)
    assert.equal(rootAfter.untracked_sha256, rootBefore.untracked_sha256)
  } finally {
    try { run('git', ['worktree', 'remove', '--force', fx.pool], fx.root) } catch { }
    await new Promise(resolveDelay => setTimeout(resolveDelay, 250))
    try {
      rmSync(fx.sandbox, { recursive: true, force: true, maxRetries: 8, retryDelay: 100 })
    } catch (error) {
      if (error?.code !== 'EPERM') throw error
      const cleanup = spawn(process.execPath, ['-e', `setTimeout(()=>require('fs').rmSync(${JSON.stringify(fx.sandbox)},{recursive:true,force:true,maxRetries:20,retryDelay:200}),1000)`], { detached: true, stdio: 'ignore', windowsHide: true })
      cleanup.unref()
    }
  }
})

test('Stage 1 recovery gate failure preserves the committed lane and writes no journal', { timeout: 120_000 }, async () => {
  const fx = fixture({ failGate: 'ingest_manifest', requireCommittedHead: true })
  const identity = { operation_id: 'bootstrap_data', stage: 'bootstrap-1', run_id: 'run-partial', branch: 'lane/oracle-bootstrap-data-run-partial', base_sha: fx.base, heartbeat_id: 'run-partial-data' }
  try {
    const acquire = fx.broker.openLane(identity)
    assert.throws(() => fx.broker.recoverStage1({ capability: acquire.capability }), /fixed gate failed/)
    const failedSha = git(fx.pool, 'rev-parse', 'HEAD')
    const failedTree = git(fx.pool, 'show', '-s', '--format=%T', failedSha)
    assert.notEqual(failedSha, fx.base)
    assert.equal(git(fx.pool, 'show', '-s', '--format=%P', failedSha), fx.base)
    assert.deepEqual(git(fx.pool, 'diff', '--name-only', `${fx.base}...${failedSha}`).split(/\r?\n/).filter(Boolean).sort(), Object.keys(fx.files).sort())
    assert.deepEqual(git(fx.pool, 'status', '--porcelain=v1', '-uall').split(/\r?\n/).filter(line => line && !line.includes('.atx-lease')), [])
    const recoveryDir = join(git(fx.root, 'rev-parse', '--absolute-git-dir'), 'oracle-lane-broker-v3', 'recoveries')
    assert.deepEqual(readdirSync(recoveryDir).filter(name => name.endsWith('.json')), [])
    assert.throws(() => fx.broker.releaseLane({ capability: acquire.capability }), /failed Stage 1 gate lane/)
    const quarantine = fx.broker.quarantineLane({ capability: acquire.capability })
    assert.equal(quarantine.quarantined, true)
    assert.equal(quarantine.sha, failedSha)
    assert.equal(quarantine.tree, failedTree)
    assert.deepEqual(quarantine.preserved_paths, [])
    assert.equal(quarantine.gate_receipts.length, 4)
    assert.ok(quarantine.gate_receipts.every(receipt => receipt.tested_sha === failedSha && receipt.tested_tree === failedTree))
    assert.equal(quarantine.gate_receipts.find(receipt => receipt.gate_id === 'ingest_manifest').exit_code, 17)
    assert.equal(quarantine.gate_receipts.filter(receipt => receipt.exit_code === 0).length, 3)
    assert.equal(existsSync(join(fx.pool, '.atx-lease')), false)
    assert.equal(existsSync(join(fx.pool, '.atx-quarantine-v3')), true)
    assert.deepEqual(git(fx.pool, 'status', '--porcelain=v1', '-uall').split(/\r?\n/).filter(line => line && !line.includes('.atx-quarantine-v3')), [])
    assert.deepEqual(readdirSync(recoveryDir).filter(name => name.endsWith('.json')), [])
    assert.throws(() => fx.broker.listWorkspace({ capability: acquire.capability }), /stale|inactive/)
    assert.throws(() => fx.broker.openLane(identity), /quarantined for audit/)
    assert.equal(existsSync(join(fx.poolRoot, 'pool-2', '.atx-lease')), false)
  } finally {
    try { run('git', ['worktree', 'remove', '--force', fx.pool], fx.root) } catch { }
    await new Promise(resolveDelay => setTimeout(resolveDelay, 250))
    try {
      rmSync(fx.sandbox, { recursive: true, force: true, maxRetries: 8, retryDelay: 100 })
    } catch (error) {
      if (error?.code !== 'EPERM') throw error
      const cleanup = spawn(process.execPath, ['-e', `setTimeout(()=>require('fs').rmSync(${JSON.stringify(fx.sandbox)},{recursive:true,force:true,maxRetries:20,retryDelay:200}),1000)`], { detached: true, stdio: 'ignore', windowsHide: true })
      cleanup.unref()
    }
  }
})
