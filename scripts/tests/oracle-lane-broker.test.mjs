import test from 'node:test'
import assert from 'node:assert/strict'
import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { spawn, spawnSync } from 'node:child_process'

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

function fixture() {
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
  write(root, 'scripts/oracle-bootstrap-preflight.ps1', [
    'param([string]$Gate)',
    "$value = [ordered]@{schema_version=1;status='PASS';observations=1;command_id=$Gate;raw_output_sha256=('a' * 64)}",
    '[Console]::Out.Write(($value | ConvertTo-Json -Compress))',
  ].join('\r\n'))
  git(root, 'add', '--', '.gitignore', 'scripts', 'atx-vol')
  git(root, 'commit', '-m', 'fixture base')
  const base = git(root, 'rev-parse', 'HEAD')

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
  mkdirSync(poolRoot, { recursive: true })
  const pool = join(poolRoot, 'pool-1')
  git(root, 'worktree', 'add', '--detach', pool, base)
  mkdirSync(join(pool, 'build'), { recursive: true })
  writeFileSync(join(pool, 'build', 'build.ninja'), '# warm fixture\n', 'utf8')

  const gateRegistry = { ...GATE_REGISTRY }
  for (const gateId of ['aggregate_store', 'ingest_manifest', 'cohort_manifests', 'holdout_digest']) {
    gateRegistry[gateId] = { display: `powershell scripts\\oracle-bootstrap-preflight.ps1 -Gate ${gateId}`, file: 'scripts/oracle-bootstrap-preflight.ps1', args: ['-Gate', gateId] }
  }
  const broker = new OracleLaneBroker({ root, poolRoot, testMode: true, gateRegistry, recoverySource: { commit: sourceCommit, parent: base, tree: sourceTree, files } })
  return { sandbox, root, poolRoot, pool, base, sourceCommit, sourceTree, files, broker }
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
    'vol-lane-controller': 'tools: mcp__oracle_lane_broker__lane_open, mcp__oracle_lane_broker__lane_release',
    'vol-builder': 'tools: mcp__oracle_lane_broker__repo_search, mcp__oracle_lane_broker__repo_read, mcp__oracle_lane_broker__workspace_list, mcp__oracle_lane_broker__patch_apply, mcp__oracle_lane_broker__gate_run, mcp__oracle_lane_broker__lane_commit, mcp__oracle_lane_broker__recover_stage1',
    'vol-verifier': 'tools: mcp__oracle_lane_broker__repo_search, mcp__oracle_lane_broker__repo_read, mcp__oracle_lane_broker__workspace_list, mcp__oracle_lane_broker__patch_apply, mcp__oracle_lane_broker__gate_run, mcp__oracle_lane_broker__lane_commit, mcp__oracle_lane_broker__lane_integrate, mcp__oracle_lane_broker__canonical_audit',
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
  for (const agentName of ['vol-lane-controller', 'vol-builder', 'vol-reviewer', 'vol-verifier', 'vol-ref-finalizer', 'vol-ref-auditor']) {
    assert.match(oracleWorkflow, new RegExp(`agentType: '${agentName}'`))
  }
  const sprintWorkflow = readFileSync(join(projectRoot, '.claude', 'workflows', 'vol-sprint.js'), 'utf8')
  assert.ok(sprintWorkflow.indexOf("failure: 'ORACLE_BROKER_MIGRATION_REQUIRED'") < sprintWorkflow.indexOf('const BASE_REF'))
})

test('temp repo broker rejects incident classes and proves recovery, lane commit/release, integration, and canonical CAS', { timeout: 120_000 }, async () => {
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
    assert.throws(() => fx.broker.applyPatch({ capability: acquire.capability, patch: escapePatch }), /unsafe|outside|escapes/)
    const gitPatch = 'diff --git a/.git/config b/.git/config\n--- a/.git/config\n+++ b/.git/config\n@@ -1 +1 @@\n-x\n+y\n'
    assert.throws(() => fx.broker.applyPatch({ capability: acquire.capability, patch: gitPatch }), /unsafe/)
    assert.throws(() => fx.broker.runGate({ capability: acquire.capability, gate_id: 'full-suite' }), /fixed broker registry/)

    const leasePath = join(fx.pool, '.atx-lease')
    const lease = readFileSync(leasePath, 'ascii')
    writeFileSync(leasePath, lease.replace(/keeper_pid=\d+/, 'keeper_pid=99999999'), 'ascii')
    assert.throws(() => fx.broker.listWorkspace({ capability: acquire.capability }), /lease mismatch/)
    writeFileSync(leasePath, lease, 'ascii')

    const recovered = fx.broker.recoverStage1({ capability: acquire.capability })
    assert.equal(recovered.recovery.source_commit, fx.sourceCommit)
    assert.equal(recovered.recovery.source_parent, fx.base)
    assert.equal(recovered.recovery.source_tree, fx.sourceTree)
    assert.equal(recovered.recovery.adoption_rerun, false)
    assert.deepEqual(recovered.files_changed.sort(), Object.keys(fx.files).sort())
    assert.deepEqual(recovered.gate_receipts.map(item => item.gate_id), ['aggregate_store', 'ingest_manifest', 'cohort_manifests', 'holdout_digest'])
    assert.ok(recovered.gate_receipts.every(item => item.exit_code === 0 && item.broker_evidence.physical_cwd === fx.pool))
    assert.throws(() => fx.broker.recoverStage1({ capability: acquire.capability }), /not pristine/)

    const inspection = fx.broker.inspectCommit({ base_sha: fx.base, candidate_sha: recovered.sha })
    assert.deepEqual(inspection.paths.sort(), Object.keys(fx.files).sort())
    assert.equal(inspection.broker_evidence.root_guard_before.main_sha, fx.base)
    const released = fx.broker.releaseLane({ capability: acquire.capability })
    assert.equal(released.finalize_capability, '')
    assert.equal(released.sha, recovered.sha)
    assert.throws(() => fx.broker.listWorkspace({ capability: acquire.capability }), /stale|inactive/)

    const integration = fx.broker.openLane({ operation_id: 'bootstrap_integration', stage: 'bootstrap-prepare', run_id: 'run-stage1', branch: 'integration/oracle-bootstrap-data-run-stage1', base_sha: fx.base, heartbeat_id: 'run-stage1-integration' })
    const integrated = fx.broker.integrate({ capability: integration.capability, reviewed_shas: [recovered.sha] })
    assert.equal(integrated.sha, recovered.sha)
    assert.equal(integrated.head_receipt.sha, recovered.sha)
    for (const gateId of ['aggregate_store', 'ingest_manifest', 'cohort_manifests', 'holdout_digest']) assert.equal(fx.broker.runGate({ capability: integration.capability, gate_id: gateId }).exit_code, 0)
    const integrationRelease = fx.broker.releaseLane({ capability: integration.capability })
    assert.match(integrationRelease.finalize_capability, /^[0-9a-f]{64}$/)

    const finalized = fx.broker.canonicalFinalize({ finalize_capability: integrationRelease.finalize_capability })
    assert.equal(finalized.ref, CANONICAL_REF)
    assert.equal(finalized.new_sha, recovered.sha)
    assert.equal(finalized.expected_old_sha, '0'.repeat(40))
    assert.equal(finalized.broker_evidence.root_guard_before.main_sha, finalized.broker_evidence.root_guard_after.main_sha)
    assert.notEqual(finalized.broker_evidence.root_guard_before.canonical_sha, finalized.broker_evidence.root_guard_after.canonical_sha)
    assert.throws(() => fx.broker.canonicalFinalize({ finalize_capability: integrationRelease.finalize_capability }), /unknown|stale|consumed/)
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
