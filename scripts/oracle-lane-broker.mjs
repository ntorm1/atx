#!/usr/bin/env node

// Trusted mutation boundary for the SpiderRock oracle loop.
//
// The MCP surface intentionally has no raw command tool.  Every child process is
// selected from the registries below and every mutating call is rooted in a
// keeper-backed pool worktree.  Capabilities are opaque random identifiers whose
// durable records bind the complete v3 lease identity.

import { createHash, createHmac, randomBytes, timingSafeEqual } from 'node:crypto'
import { existsSync, lstatSync, mkdirSync, readFileSync, readdirSync, realpathSync, renameSync, rmSync, statSync, writeFileSync } from 'node:fs'
import { dirname, isAbsolute, join, relative, resolve, sep } from 'node:path'
import { fileURLToPath } from 'node:url'
import { spawnSync } from 'node:child_process'
import readline from 'node:readline'

const VERSION = 3
const CANONICAL_REF = 'refs/heads/oracle/canonical'
const MAIN_REF = 'refs/heads/main'
const ZERO_SHA = '0000000000000000000000000000000000000000'
const SHA_RE = /^[0-9a-f]{40}$/
const SHA256_RE = /^[0-9a-f]{64}$/
const SAFE_ID_RE = /^[A-Za-z0-9][A-Za-z0-9._-]{0,191}$/
const SAFE_BRANCH_RE = /^(?:lane|integration)\/[A-Za-z0-9][A-Za-z0-9._\/-]{0,220}$/
const RECOVERY_SOURCE = Object.freeze({
  commit: '58a94584baabae8263d16421f633540b420de10b',
  parent: '3025895fea5f569c098090015d90b8b206e8d5a1',
  tree: '6a64d8df30456b1dc4ca1e244f29a7affb77c786',
  files: Object.freeze({
    'atx-vol/bench/oracle/bootstrap/data.json': 'bb7ce65e891f8f417f4c71af0769ac84b20531fa',
    'atx-vol/bench/oracle/cohorts/holdout.sha256': '66e49a2b4e8835b97e6c2c3d546f345dc751bad0',
  }),
})

const CONVENTION_CHANGE_PATHS = Object.freeze([
  '.claude/workflows/vol-oracle-iter.js',
  'atx-vol/CHANGELOG.md',
  'atx-vol/CMakeLists.txt',
  'atx-vol/bench/oracle/CHARTER.md',
  'atx-vol/bench/oracle/CONVENTIONS.md',
  'atx-vol/bench/oracle/bootstrap/conventions.json',
  'atx-vol/bench/oracle/scorecards/iter-000.json',
  'atx-vol/tests/CMakeLists.txt',
  'atx-vol/tests/oracle_conventions_test.cpp',
  'atx-vol/tools/oracle_bench_main.cpp',
  'atx-vol/tools/oracle_convention_sweep.cpp',
  'atx-vol/tools/oracle_convention_sweep.hpp',
  'atx-vol/tools/oracle_conventions.cpp',
  'atx-vol/tools/oracle_conventions.hpp',
  'scripts/oracle-capability.ps1',
  'scripts/oracle-lane-broker.mjs',
  'scripts/oracle-targeted-gate.ps1',
  'scripts/tests/oracle-capability.Tests.ps1',
  'scripts/tests/oracle-lane-broker.test.mjs',
  'scripts/tests/oracle-targeted-gate.Tests.ps1',
  'scripts/tests/workflow-contracts.test.mjs',
])

const OPERATION_REGISTRY = Object.freeze({
  bootstrap_data: {
    stage: 'bootstrap-1', branch: /^lane\/oracle-bootstrap-data-/, finalize: false,
    exact: Object.keys(RECOVERY_SOURCE.files), prefixes: [],
  },
  bootstrap_mode_a: {
    stage: 'bootstrap-2', branch: /^lane\/oracle-bootstrap-mode-a-/, finalize: false,
    exact: ['atx-vol/bench/oracle/bootstrap/mode-a.json', 'atx-vol/CHANGELOG.md'],
    prefixes: ['atx-vol/src/pricing/', 'atx-vol/include/atx/vol/api/pricing/', 'atx-vol/tests/', 'atx-vol/bench/oracle/'],
  },
  bootstrap_conventions: {
    stage: 'bootstrap-3', branch: /^lane\/oracle-bootstrap-conventions-/, finalize: false,
    exact: CONVENTION_CHANGE_PATHS,
    prefixes: [],
  },
  bootstrap_mode_b: {
    stage: 'bootstrap-4', branch: /^lane\/oracle-bootstrap-mode-b-/, finalize: false,
    exact: ['atx-vol/bench/oracle/bootstrap/mode-b.json', 'atx-vol/CHANGELOG.md'],
    prefixes: ['atx-vol/src/pricing/', 'atx-vol/include/atx/vol/api/pricing/', 'atx-vol/tests/', 'atx-vol/bench/oracle/'],
  },
  bootstrap_integration: {
    stage: 'bootstrap-prepare', branch: /^integration\/oracle-bootstrap-/, finalize: true,
    exact: [...Object.keys(RECOVERY_SOURCE.files), 'atx-vol/bench/oracle/bootstrap/mode-a.json',
      ...CONVENTION_CHANGE_PATHS],
    prefixes: [],
  },
  measure: {
    stage: 'measure', branch: /^lane\/oracle-measure-/, finalize: false,
    exact: [], prefixes: ['atx-vol/bench/oracle/scorecards/', 'atx-vol/docs/oracle/scorecards/'],
  },
  sprint_build: {
    stage: 'improve', branch: /^lane\//, finalize: false,
    exact: ['atx-vol/CHANGELOG.md', 'atx-vol/bench/oracle/CHARTER.md'],
    prefixes: ['atx-vol/src/pricing/', 'atx-vol/include/atx/vol/api/pricing/', 'atx-vol/tests/'],
  },
  sprint_integration: {
    stage: 'improve-gate', branch: /^integration\//, finalize: false,
    exact: ['atx-vol/docs/LEDGER.md'], prefixes: ['atx-vol/'],
  },
  ratchet: {
    stage: 'ratchet', branch: /^lane\/oracle-ratchet-/, finalize: true,
    exact: ['atx-vol/docs/LEDGER.md', 'atx-vol/docs/oracle/NORTHSTAR.md'],
    prefixes: ['atx-vol/bench/oracle/scorecards/', 'atx-vol/docs/oracle/scorecards/'],
  },
})

const GATE_REGISTRY = Object.freeze({
  disk: { display: 'powershell scripts\\oracle-bootstrap-preflight.ps1 -Gate disk', file: 'scripts/oracle-bootstrap-preflight.ps1', args: ['-Gate', 'disk'] },
  aggregate_store: { display: 'powershell scripts\\oracle-bootstrap-preflight.ps1 -Gate aggregate_store', file: 'scripts/oracle-bootstrap-preflight.ps1', args: ['-Gate', 'aggregate_store'] },
  ingest_manifest: { display: 'powershell scripts\\oracle-bootstrap-preflight.ps1 -Gate ingest_manifest', file: 'scripts/oracle-bootstrap-preflight.ps1', args: ['-Gate', 'ingest_manifest'] },
  cohort_manifests: { display: 'powershell scripts\\oracle-bootstrap-preflight.ps1 -Gate cohort_manifests', file: 'scripts/oracle-bootstrap-preflight.ps1', args: ['-Gate', 'cohort_manifests'] },
  holdout_digest: { display: 'powershell scripts\\oracle-bootstrap-preflight.ps1 -Gate holdout_digest', file: 'scripts/oracle-bootstrap-preflight.ps1', args: ['-Gate', 'holdout_digest'] },
  mode_a_targeted_tests: { display: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate mode_a_targeted_tests', file: 'scripts/oracle-targeted-gate.ps1', args: ['-Gate', 'mode_a_targeted_tests'] },
  mode_a_smoke: { display: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate mode_a_smoke', file: 'scripts/oracle-targeted-gate.ps1', args: ['-Gate', 'mode_a_smoke'] },
  convention_tests: { display: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate convention_tests', file: 'scripts/oracle-targeted-gate.ps1', args: ['-Gate', 'convention_tests'] },
  mode_a_smoke_tune: { display: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate mode_a_smoke_tune', file: 'scripts/oracle-targeted-gate.ps1', args: ['-Gate', 'mode_a_smoke_tune'] },
  residual_floor: { display: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate residual_floor', file: 'scripts/oracle-targeted-gate.ps1', args: ['-Gate', 'residual_floor'] },
  convention_speed_measure: { display: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate convention_speed_measure', file: 'scripts/oracle-targeted-gate.ps1', args: ['-Gate', 'convention_speed_measure'] },
  convention_speed: { display: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate convention_speed', file: 'scripts/oracle-targeted-gate.ps1', args: ['-Gate', 'convention_speed'] },
  mode_b_targeted_tests: { display: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate mode_b_targeted_tests', file: 'scripts/oracle-targeted-gate.ps1', args: ['-Gate', 'mode_b_targeted_tests'] },
  mode_b_smoke_tune: { display: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate mode_b_smoke_tune', file: 'scripts/oracle-targeted-gate.ps1', args: ['-Gate', 'mode_b_smoke_tune'] },
  sprint_american_greeks_delta_put: { display: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate sprint_american_greeks_delta_put', file: 'scripts/oracle-targeted-gate.ps1', args: ['-Gate', 'sprint_american_greeks_delta_put'] },
  sprint_adjusted_greeks_flat_smile: { display: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate sprint_adjusted_greeks_flat_smile', file: 'scripts/oracle-targeted-gate.ps1', args: ['-Gate', 'sprint_adjusted_greeks_flat_smile'] },
  measure_mode_a: { display: 'atx-vol-oracle-bench --cohort smoke,tune --mode A --scorecard --aggregate-only', exe: 'atx-vol-oracle-bench', args: ['--cohort', 'smoke,tune', '--mode', 'A', '--scorecard', '--aggregate-only'] },
  measure_mode_b: { display: 'atx-vol-oracle-bench --cohort smoke,tune --mode B --scorecard --aggregate-only', exe: 'atx-vol-oracle-bench', args: ['--cohort', 'smoke,tune', '--mode', 'B', '--scorecard', '--aggregate-only'] },
  measure_speed: { display: 'atx-vol-oracle-bench --cohort tune --benchmark-speed --preset rel-avx2 --quiet-host --aggregate-only', exe: 'atx-vol-oracle-bench', args: ['--cohort', 'tune', '--benchmark-speed', '--preset', 'rel-avx2', '--quiet-host', '--aggregate-only'] },
  holdout_mode_a: { display: 'atx-vol-oracle-bench --cohort holdout --mode A --aggregate-only', exe: 'atx-vol-oracle-bench', args: ['--cohort', 'holdout', '--mode', 'A', '--aggregate-only'] },
  holdout_mode_b: { display: 'atx-vol-oracle-bench --cohort holdout --mode B --aggregate-only', exe: 'atx-vol-oracle-bench', args: ['--cohort', 'holdout', '--mode', 'B', '--aggregate-only'] },
  rel_avx2_speed: { display: 'atx-vol-oracle-bench --cohort tune --benchmark-speed --preset rel-avx2 --quiet-host --aggregate-only', exe: 'atx-vol-oracle-bench', args: ['--cohort', 'tune', '--benchmark-speed', '--preset', 'rel-avx2', '--quiet-host', '--aggregate-only'] },
  'unit-build:atx-vol-tests': { display: 'powershell scripts\\atx-build.ps1 -Preset dev build atx-vol-tests', file: 'scripts/atx-build.ps1', args: ['-Preset', 'dev', 'build', 'atx-vol-tests'] },
  'unit-test:^AmericanGreeks.Delta_MatchesFd_Put$': { display: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate sprint_american_greeks_delta_put', file: 'scripts/oracle-targeted-gate.ps1', args: ['-Gate', 'sprint_american_greeks_delta_put'] },
  'unit-test:^AdjustedGreeks.FlatSmileLeavesDeltaUnchanged$': { display: 'powershell scripts\\oracle-targeted-gate.ps1 -Gate sprint_adjusted_greeks_flat_smile', file: 'scripts/oracle-targeted-gate.ps1', args: ['-Gate', 'sprint_adjusted_greeks_flat_smile'] },
  'pch-off:atx-vol-tests': { display: 'powershell scripts\\atx-build.ps1 -Preset hygiene build atx-vol-tests', file: 'scripts/atx-build.ps1', args: ['-Preset', 'hygiene', 'build', 'atx-vol-tests'] },
  'scorecard:mode_a_smoke_tune': { display: 'atx-vol-oracle-bench --cohort smoke,tune --mode A --scorecard --aggregate-only', exe: 'atx-vol-oracle-bench', args: ['--cohort', 'smoke,tune', '--mode', 'A', '--scorecard', '--aggregate-only'] },
  'scorecard:mode_b_smoke_tune': { display: 'atx-vol-oracle-bench --cohort smoke,tune --mode B --scorecard --aggregate-only', exe: 'atx-vol-oracle-bench', args: ['--cohort', 'smoke,tune', '--mode', 'B', '--scorecard', '--aggregate-only'] },
  'speed:rel_avx2_quiet': { display: 'atx-vol-oracle-bench --cohort tune --benchmark-speed --preset rel-avx2 --quiet-host --aggregate-only', exe: 'atx-vol-oracle-bench', args: ['--cohort', 'tune', '--benchmark-speed', '--preset', 'rel-avx2', '--quiet-host', '--aggregate-only'] },
  'oracle-test:american-rsi': { display: 'atx-vol-oracle-bench --cohort smoke,tune --test american-rsi --aggregate-only', exe: 'atx-vol-oracle-bench', args: ['--cohort', 'smoke,tune', '--test', 'american-rsi', '--aggregate-only'] },
  'oracle-test:american-price-rsi': { display: 'atx-vol-oracle-bench --cohort smoke,tune --test american-price-rsi --aggregate-only', exe: 'atx-vol-oracle-bench', args: ['--cohort', 'smoke,tune', '--test', 'american-price-rsi', '--aggregate-only'] },
  'oracle-test:american-greeks-rsi': { display: 'atx-vol-oracle-bench --cohort smoke,tune --test american-greeks-rsi --aggregate-only', exe: 'atx-vol-oracle-bench', args: ['--cohort', 'smoke,tune', '--test', 'american-greeks-rsi', '--aggregate-only'] },
  'oracle-test:american-simd-rsi': { display: 'atx-vol-oracle-bench --cohort smoke,tune --test american-simd-rsi --aggregate-only', exe: 'atx-vol-oracle-bench', args: ['--cohort', 'smoke,tune', '--test', 'american-simd-rsi', '--aggregate-only'] },
})

const MESSAGE_REGISTRY = Object.freeze({
  bootstrap_data_recovery: 'feat(vol-oracle): recover validated Stage 1 receipts',
  bootstrap_mode_a: 'feat(vol-oracle): bootstrap Mode A oracle bench',
  bootstrap_conventions: 'feat(vol-oracle): pin oracle convention floor',
  bootstrap_mode_b: 'feat(vol-oracle): bootstrap Mode B oracle bench',
  sprint_lane: 'feat(vol-oracle): apply RSI hypothesis',
  sprint_integration: 'chore(vol-oracle): integrate reviewed RSI lanes',
  measure: 'chore(vol-oracle): record aggregate measurement',
  ratchet: 'chore(vol-oracle): ratchet scorecard and memory',
})

const BOOTSTRAP_INTEGRATION_GATES = Object.freeze({
  bootstrap_data: Object.freeze(['aggregate_store', 'ingest_manifest', 'cohort_manifests', 'holdout_digest']),
  bootstrap_mode_a: Object.freeze(['mode_a_targeted_tests', 'mode_a_smoke']),
  bootstrap_conventions: Object.freeze(['convention_tests', 'mode_a_smoke_tune', 'residual_floor', 'convention_speed_measure', 'convention_speed']),
  bootstrap_mode_b: Object.freeze(['mode_b_targeted_tests', 'mode_b_smoke_tune']),
})

const sha256 = value => createHash('sha256').update(value).digest('hex')
export const brokerGateOutputSha256 = output => sha256(Buffer.from(String(output), 'utf8'))
export function brokerGateReceiptId(operationId, receipt) {
  return sha256(Buffer.from(JSON.stringify({
    operation_id: operationId,
    gate_id: receipt.gate_id,
    tested_sha: receipt.tested_sha,
    tested_tree: receipt.tested_tree,
    command: receipt.command,
    exit_code: receipt.exit_code,
    raw_output_sha256: receipt.raw_output_sha256,
  }), 'utf8'))
}
export function buildBrokerGateReceipt(operationId, receipt) {
  const brokerEvidence = { ...receipt.broker_evidence, raw_output_sha256: brokerGateOutputSha256(receipt.output) }
  const receiptId = brokerGateReceiptId(operationId, {
    gate_id: receipt.gate_id, tested_sha: receipt.tested_sha, tested_tree: receipt.tested_tree,
    command: receipt.command, exit_code: receipt.exit_code, raw_output_sha256: brokerEvidence.raw_output_sha256,
  })
  return {
    receipt_id: receiptId, gate_id: receipt.gate_id, tested_sha: receipt.tested_sha, tested_tree: receipt.tested_tree,
    command: receipt.command, exit_code: receipt.exit_code, output: receipt.output, broker_evidence: brokerEvidence,
  }
}
const lower = value => String(value).toLowerCase()
const samePath = (left, right) => lower(resolve(left)) === lower(resolve(right))

function assertObject(value, name = 'input') {
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error(`${name} must be an object`)
}

function assertExactKeys(value, allowed, required = allowed) {
  assertObject(value)
  for (const key of Object.keys(value)) if (!allowed.includes(key)) throw new Error(`unexpected input field: ${key}`)
  for (const key of required) if (!(key in value)) throw new Error(`missing input field: ${key}`)
}

function normalizeRel(value) {
  if (typeof value !== 'string' || !value || value.includes('\0') || isAbsolute(value)) throw new Error('path must be a nonempty repository-relative path')
  const rel = value.replace(/\\/g, '/').replace(/^\.\//, '')
  if (rel.startsWith('/') || rel.split('/').some(part => !part || part === '.' || part === '..') || rel.toLowerCase() === '.git' || rel.toLowerCase().startsWith('.git/')) throw new Error(`unsafe repository path: ${value}`)
  return rel
}

function assertNoReparse(root, target, allowMissingLeaf = false) {
  const absoluteRoot = resolve(root)
  const absoluteTarget = resolve(target)
  const rel = relative(absoluteRoot, absoluteTarget)
  if (rel === '..' || rel.startsWith(`..${sep}`) || isAbsolute(rel)) throw new Error('path escapes trusted root')
  if (!samePath(realpathSync.native(absoluteRoot), absoluteRoot)) throw new Error('trusted root is a symlink or reparse alias')
  let cursor = absoluteRoot
  for (const part of rel.split(/[\\/]/).filter(Boolean)) {
    cursor = join(cursor, part)
    if (!existsSync(cursor)) {
      if (allowMissingLeaf) continue
      throw new Error(`path does not exist: ${cursor}`)
    }
    const stat = lstatSync(cursor)
    if (stat.isSymbolicLink()) throw new Error(`symlink/reparse path rejected: ${cursor}`)
    if (!samePath(realpathSync.native(cursor), cursor)) throw new Error(`reparse alias rejected: ${cursor}`)
  }
  return absoluteTarget
}

function parseRecord(text) {
  const result = {}
  for (const line of String(text).split(/\r?\n/)) {
    const split = line.indexOf('=')
    if (split > 0) result[line.slice(0, split)] = line.slice(split + 1)
  }
  return result
}

function processResult(exe, args, cwd, input) {
  const result = spawnSync(exe, args, { cwd, input, encoding: 'utf8', windowsHide: true, maxBuffer: 32 * 1024 * 1024, env: { ...process.env, GIT_CONFIG_COUNT: '1', GIT_CONFIG_KEY_0: 'safe.directory', GIT_CONFIG_VALUE_0: '*' } })
  const stdout = String(result.stdout || '')
  const stderr = String(result.stderr || '')
  return { exit_code: Number.isInteger(result.status) ? result.status : 1, output: `${stdout}${stderr}`.trim(), stdout, stderr, error: result.error ? String(result.error.message || result.error) : '' }
}

function requireSuccess(result, label) {
  if (result.exit_code !== 0) throw new Error(`${label} failed (${result.exit_code}): ${result.output || result.error}`)
  return result
}

export class OracleLaneBroker {
  constructor(options = {}) {
    const scriptRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..')
    this.root = resolve(options.root || scriptRoot)
    this.poolRoot = resolve(options.poolRoot || join(dirname(this.root), 'atx-wt'))
    this.testMode = options.testMode === true
    this.gates = options.gateRegistry || GATE_REGISTRY
    this.recoverySource = options.recoverySource || RECOVERY_SOURCE
    assertNoReparse(this.root, this.root)
    this.gitDir = this.#git(['rev-parse', '--absolute-git-dir'], this.root).stdout.trim()
    if (!existsSync(this.gitDir)) throw new Error('canonical git directory missing')
    this.stateDir = join(this.gitDir, 'oracle-lane-broker-v3')
    this.capDir = join(this.stateDir, 'capabilities')
    this.finalizeDir = join(this.stateDir, 'finalizers')
    this.recoveryDir = join(this.stateDir, 'recoveries')
    mkdirSync(this.capDir, { recursive: true })
    mkdirSync(this.finalizeDir, { recursive: true })
    mkdirSync(this.recoveryDir, { recursive: true })
    const secretPath = join(this.stateDir, 'secret')
    if (!existsSync(secretPath)) writeFileSync(secretPath, randomBytes(32).toString('hex'), { encoding: 'ascii', mode: 0o600, flag: 'wx' })
    this.secret = readFileSync(secretPath, 'ascii').trim()
    if (!/^[0-9a-f]{64}$/.test(this.secret)) throw new Error('broker secret is invalid')
  }

  #git(args, cwd = this.root, input) {
    return requireSuccess(processResult('git', args, cwd, input), `git ${args[0]}`)
  }

  #ref(ref) {
    const result = processResult('git', ['rev-parse', '--verify', `${ref}^{commit}`], this.root)
    if (result.exit_code !== 0) return null
    const sha = result.stdout.trim().toLowerCase()
    return SHA_RE.test(sha) ? sha : null
  }

  rootGuard() {
    const reportedIndexPath = this.#git(['rev-parse', '--git-path', 'index'], this.root).stdout.trim()
    const indexPath = isAbsolute(reportedIndexPath) ? reportedIndexPath : resolve(this.root, reportedIndexPath)
    const indexBytes = existsSync(indexPath) ? readFileSync(indexPath) : Buffer.alloc(0)
    const trackedA = processResult('git', ['diff', '--raw', '-z'], this.root)
    const trackedB = processResult('git', ['diff', '--cached', '--raw', '-z'], this.root)
    const untracked = requireSuccess(processResult('git', ['ls-files', '--others', '--exclude-standard', '-z'], this.root), 'root untracked snapshot')
    requireSuccess(trackedA, 'root tracked snapshot')
    requireSuccess(trackedB, 'root index snapshot')
    const guard = {
      main_sha: this.#ref(MAIN_REF),
      canonical_sha: this.#ref(CANONICAL_REF),
      index_sha256: sha256(indexBytes),
      tracked_sha256: sha256(Buffer.from(`${trackedA.stdout}\0${trackedB.stdout}`, 'utf8')),
      untracked_sha256: sha256(Buffer.from(untracked.stdout, 'utf8')),
    }
    guard.raw_sha256 = sha256(Buffer.from(JSON.stringify(guard), 'utf8'))
    return guard
  }

  #assertGuard(before, after, allowCanonical = false) {
    for (const key of ['main_sha', 'index_sha256', 'tracked_sha256', 'untracked_sha256']) if (before[key] !== after[key]) throw new Error(`canonical-root guard changed: ${key}`)
    if (!allowCanonical && before.canonical_sha !== after.canonical_sha) throw new Error('canonical-root guard changed: canonical_sha')
  }

  #evidence(logicalOperation, physicalCwd, display, result, before, after) {
    return {
      logical_operation: logicalOperation,
      physical_cwd: resolve(physicalCwd),
      command: display,
      exit_code: result.exit_code,
      output: result.output,
      raw_output_sha256: sha256(Buffer.from(`${result.stdout || ''}${result.stderr || ''}`, 'utf8')),
      root_guard_before: before,
      root_guard_after: after,
    }
  }

  #capPath(token) {
    if (!/^[0-9a-f]{64}$/.test(String(token || ''))) throw new Error('invalid opaque capability')
    return join(this.capDir, `${token}.json`)
  }

  #finalizePath(token) {
    if (!/^[0-9a-f]{64}$/.test(String(token || ''))) throw new Error('invalid opaque finalize capability')
    return join(this.finalizeDir, `${token}.json`)
  }

  #seal(record) {
    const body = JSON.stringify(record)
    return { ...record, seal: createHmac('sha256', this.secret).update(body).digest('hex') }
  }

  #verifySealed(record) {
    assertObject(record, 'capability record')
    const { seal, ...body } = record
    const expected = createHmac('sha256', this.secret).update(JSON.stringify(body)).digest()
    const actual = Buffer.from(String(seal || ''), 'hex')
    if (actual.length !== expected.length || !timingSafeEqual(actual, expected)) throw new Error('capability seal mismatch')
    return body
  }

  #saveCap(token, body) {
    const target = this.#capPath(token)
    const temp = `${target}.${randomBytes(8).toString('hex')}.tmp`
    writeFileSync(temp, JSON.stringify(this.#seal(body)), { encoding: 'utf8', flag: 'wx' })
    if (existsSync(target)) rmSync(target)
    renameSync(temp, target)
  }

  #loadCap(token, requireActive = true) {
    const target = this.#capPath(token)
    if (!existsSync(target)) throw new Error('unknown opaque capability')
    const cap = this.#verifySealed(JSON.parse(readFileSync(target, 'utf8')))
    if (cap.version !== VERSION || (requireActive && cap.state !== 'active')) throw new Error('capability is stale or inactive')
    if (requireActive) this.#revalidateLease(cap)
    return cap
  }

  #saveFinalize(token, body) {
    const target = this.#finalizePath(token)
    writeFileSync(target, JSON.stringify(this.#seal(body)), { encoding: 'utf8', flag: 'wx' })
  }

  #loadFinalize(token) {
    const target = this.#finalizePath(token)
    if (!existsSync(target)) throw new Error('unknown opaque finalize capability')
    const body = this.#verifySealed(JSON.parse(readFileSync(target, 'utf8')))
    if (body.version !== VERSION || body.state !== 'active') throw new Error('finalize capability is stale or consumed')
    return body
  }

  #recoveryIdentity(value) {
    const identity = {
      version: VERSION, operation_id: 'bootstrap_data', stage: 'bootstrap-1',
      run_id: String(value.run_id || ''), branch: String(value.branch || ''),
      base_sha: lower(value.base_sha || ''), heartbeat_id: String(value.heartbeat_id || ''),
      source_commit: this.recoverySource.commit,
    }
    if (!SAFE_ID_RE.test(identity.run_id) || !SAFE_ID_RE.test(identity.heartbeat_id) || !SAFE_BRANCH_RE.test(identity.branch) ||
        !OPERATION_REGISTRY.bootstrap_data.branch.test(identity.branch) || !SHA_RE.test(identity.base_sha)) throw new Error('invalid deterministic recovery identity')
    return identity
  }

  #recoveryPath(value) {
    const identity = this.#recoveryIdentity(value)
    const key = sha256(Buffer.from(JSON.stringify(identity), 'utf8'))
    return { identity, key, path: join(this.recoveryDir, `${key}.json`) }
  }

  #loadRecovery(value, allowMissing = true) {
    const located = this.#recoveryPath(value)
    if (!existsSync(located.path)) {
      if (allowMissing) return null
      throw new Error('sealed Stage 1 recovery result is missing')
    }
    const body = this.#verifySealed(JSON.parse(readFileSync(located.path, 'utf8')))
    if (body.state !== 'sealed' || body.recovery_id !== located.key || JSON.stringify(body.identity) !== JSON.stringify(located.identity) || !body.result) throw new Error('sealed Stage 1 recovery result identity mismatch')
    return body
  }

  #saveRecovery(value, result) {
    const located = this.#recoveryPath(value)
    if (existsSync(located.path)) {
      const existing = this.#loadRecovery(value, false)
      if (existing.result.sha !== result.sha || existing.result.tree !== result.tree) throw new Error('sealed Stage 1 recovery result conflicts with existing journal')
      return existing
    }
    const temp = `${located.path}.${randomBytes(8).toString('hex')}.tmp`
    const body = { version: VERSION, state: 'sealed', recovery_id: located.key, identity: located.identity, result }
    writeFileSync(temp, JSON.stringify(this.#seal(body)), { encoding: 'utf8', flag: 'wx' })
    renameSync(temp, located.path)
    return body
  }

  #assertRecoveryJournal(journal) {
    if (!journal || journal.identity.operation_id !== 'bootstrap_data' || journal.identity.stage !== 'bootstrap-1') throw new Error('sealed Stage 1 recovery operation mismatch')
    const result = journal.result
    if (result.recovery_id !== journal.recovery_id || result.replayed !== false || !SHA_RE.test(result.sha || '') || !SHA_RE.test(result.tree || '') ||
        this.#ref(result.sha) !== result.sha || this.#tree(result.sha) !== result.tree) throw new Error('sealed Stage 1 recovery SHA/tree mismatch')
    if (!this.#isAncestor(this.recoverySource.parent, journal.identity.base_sha) || result.recovery?.source_commit !== this.recoverySource.commit ||
        result.recovery?.source_parent !== this.recoverySource.parent || result.recovery?.source_tree !== this.recoverySource.tree || result.recovery?.adoption_rerun !== false) throw new Error('sealed Stage 1 recovery source mismatch')
    const paths = Object.keys(this.recoverySource.files).sort()
    if (JSON.stringify([...(result.files_changed || [])].sort()) !== JSON.stringify(paths) || JSON.stringify([...(result.changed_path_receipt?.paths || [])].sort()) !== JSON.stringify(paths) ||
        result.changed_path_receipt?.base_sha !== journal.identity.base_sha || result.changed_path_receipt?.tested_sha !== result.sha) throw new Error('sealed Stage 1 recovery path closure mismatch')
    for (const path of paths) {
      const proof = result.recovery.blobs?.find(item => item.path === path)
      if (!proof || proof.source_blob !== this.recoverySource.files[path] || proof.replay_blob !== this.recoverySource.files[path]) throw new Error(`sealed Stage 1 recovery blob mismatch: ${path}`)
    }
    const gates = ['aggregate_store', 'ingest_manifest', 'cohort_manifests', 'holdout_digest']
    if (!Array.isArray(result.gate_receipts) || result.gate_receipts.length !== gates.length ||
        new Set(result.gate_receipts.map(receipt => receipt.receipt_id)).size !== gates.length) throw new Error('sealed Stage 1 recovery gate set mismatch')
    for (const gate of gates) {
      const matches = result.gate_receipts.filter(receipt => receipt.gate_id === gate && receipt.exit_code === 0 && /^[0-9a-f]{64}$/.test(receipt.receipt_id || ''))
      const receipt = matches[0]
      const evidence = receipt?.broker_evidence
      if (matches.length !== 1 || receipt.command !== this.gates[gate].display || receipt.tested_sha !== result.sha || receipt.tested_tree !== result.tree ||
          !evidence || evidence.logical_operation !== `gate:${gate}` || evidence.command !== receipt.command || evidence.output !== receipt.output || evidence.exit_code !== receipt.exit_code ||
          !SHA256_RE.test(evidence.raw_output_sha256 || '') || evidence.root_guard_before?.main_sha !== evidence.root_guard_after?.main_sha ||
          evidence.root_guard_before?.index_sha256 !== evidence.root_guard_after?.index_sha256 || evidence.root_guard_before?.tracked_sha256 !== evidence.root_guard_after?.tracked_sha256 ||
          evidence.root_guard_before?.untracked_sha256 !== evidence.root_guard_after?.untracked_sha256) throw new Error(`sealed Stage 1 recovery gate mismatch: ${gate}`)
    }
    const branchSha = this.#ref(`refs/heads/${journal.identity.branch}`)
    if (branchSha !== result.sha) throw new Error('sealed Stage 1 recovery branch is missing or ahead')
    return journal
  }

  #leasePath(leaseName) {
    if (!/^pool-[0-9]+$/.test(leaseName)) throw new Error('invalid lease name')
    const lane = assertNoReparse(this.poolRoot, join(this.poolRoot, leaseName))
    return { lane, record: join(lane, '.atx-lease') }
  }

  #revalidateLease(cap, { requireLiveKeeper = true, requireCheckedOutBranch = true } = {}) {
    const { lane, record } = this.#leasePath(cap.lease_name)
    if (!samePath(lane, cap.worktree) || !existsSync(record)) throw new Error('lease path/record missing')
    const lease = parseRecord(readFileSync(record, 'ascii'))
    const fields = ['lease_token', 'run_id', 'branch', 'heartbeat_id', 'keeper_pid', 'keeper_process_started_utc', 'keeper_ready_utc']
    for (const field of fields) if (String(lease[field] || '') !== String(cap[field] || '')) throw new Error(`stale capability lease mismatch: ${field}`)
    if (lower(lease.base_sha || '') !== lower(cap.lease_start_sha || cap.base_sha)) throw new Error('stale capability lease mismatch: base_sha')
    if (lease.version !== String(VERSION) || lease.owner_kind !== 'heartbeat') throw new Error('lease is not a keeper-backed v3 lease')
    if (requireLiveKeeper) {
      const heartbeat = assertNoReparse(this.poolRoot, join(this.poolRoot, '.atx-heartbeats', `${cap.heartbeat_id}.heartbeat`))
      const age = Date.now() - statSync(heartbeat).mtimeMs
      if (age < 0 || age > Number(lease.heartbeat_timeout_seconds) * 1000) throw new Error('lease heartbeat is stale')
      const pid = Number(cap.keeper_pid)
      if (!Number.isInteger(pid) || pid <= 0) throw new Error('invalid keeper pid')
      const ps = processResult('powershell', ['-NoProfile', '-Command', `(Get-Process -Id ${pid} -ErrorAction Stop).StartTime.ToUniversalTime().ToString('o',[Globalization.CultureInfo]::InvariantCulture)`], lane)
      requireSuccess(ps, 'keeper identity check')
      if (ps.stdout.trim() !== cap.keeper_process_started_utc) throw new Error('keeper PID/start mismatch')
    }
    if (requireCheckedOutBranch) {
      const branch = this.#git(['branch', '--show-current'], lane).stdout.trim()
      if (branch !== cap.branch) throw new Error('leased branch changed')
    }
    return cap
  }

  #tree(sha, cwd = this.root) {
    const tree = this.#git(['show', '-s', '--format=%T', sha], cwd).stdout.trim().toLowerCase()
    if (!SHA_RE.test(tree)) throw new Error('commit tree identity is invalid')
    return tree
  }

  #isAncestor(ancestor, descendant) {
    if (!SHA_RE.test(String(ancestor || '')) || !SHA_RE.test(String(descendant || ''))) return false
    const result = processResult('git', ['merge-base', '--is-ancestor', ancestor, descendant], this.root)
    if (result.exit_code === 0) return true
    if (result.exit_code === 1) return false
    throw new Error(`git ancestry check failed: ${result.output}`)
  }

  #allowed(cap, relPath) {
    const rel = normalizeRel(relPath)
    const spec = OPERATION_REGISTRY[cap.operation_id]
    const scoped = cap.scope_paths || []
    if (scoped.length && !scoped.includes(rel)) return false
    return spec.exact.includes(rel) || spec.prefixes.some(prefix => rel.startsWith(prefix))
  }

  #assertBootstrapCanonical(cap) {
    if (cap.operation_id !== 'bootstrap_integration' || cap.stage !== 'bootstrap-prepare') throw new Error('bootstrap canonical guard requires bootstrap integration capability')
    const current = this.#ref(CANONICAL_REF)
    if (cap.canonical_before === null) {
      if (current !== null) throw new Error('bootstrap canonical drifted from its frozen absent state')
      return ZERO_SHA
    }
    if (cap.canonical_before !== cap.base_sha || current !== cap.base_sha) throw new Error('bootstrap canonical/base precondition drifted')
    return cap.base_sha
  }

  #bootstrapCandidateOrigin(cap, sha, tree) {
    const matches = []
    for (const file of existsSync(this.capDir) ? readdirSync(this.capDir) : []) {
      if (!/^[0-9a-f]{64}\.json$/.test(file)) continue
      const token = file.slice(0, -5)
      const candidate = this.#loadCap(token, false)
      if (candidate.token_hash !== sha256(token)) throw new Error('durable capability filename/token binding mismatch')
      if (candidate.state !== 'released' || candidate.run_id !== cap.run_id || candidate.base_sha !== cap.base_sha || candidate.canonical_before !== cap.canonical_before ||
          candidate.released_head !== sha || candidate.released_tree !== tree) continue
      const expectedStage = OPERATION_REGISTRY[candidate.operation_id]?.stage
      if (!Object.hasOwn(BOOTSTRAP_INTEGRATION_GATES, candidate.operation_id) || candidate.stage !== expectedStage) throw new Error('reviewed bootstrap candidate has wrong operation/stage')
      if (this.#ref(`refs/heads/${candidate.branch}`) !== sha || this.#tree(sha) !== tree) throw new Error('reviewed bootstrap candidate branch/SHA/tree drifted')
      matches.push({ operation_id: candidate.operation_id, stage: candidate.stage, branch: candidate.branch, capability_hash: candidate.token_hash })
    }
    if (!matches.length) throw new Error('reviewed commit lacks an exact clean released bootstrap candidate')
    const identity = `${matches[0].operation_id}\0${matches[0].stage}\0${matches[0].branch}`
    if (matches.some(item => `${item.operation_id}\0${item.stage}\0${item.branch}` !== identity)) throw new Error('reviewed commit has ambiguous bootstrap operation/stage')
    if (cap.canonical_before === null && matches[0].operation_id !== 'bootstrap_data') throw new Error('absent-canonical bootstrap integration is restricted to Stage 1')
    if (cap.canonical_before !== null && matches[0].operation_id === 'bootstrap_data') throw new Error('existing-canonical bootstrap integration requires Stage 2 or later')
    return {
      operation_id: matches[0].operation_id, stage: matches[0].stage, branch: matches[0].branch,
      candidate_sha: sha, candidate_tree: tree,
      capability_hashes: matches.map(item => item.capability_hash).sort(),
    }
  }

  #assertBootstrapGateSet(cap) {
    const origin = cap.bootstrap_origin
    if (!origin || origin.candidate_sha !== cap.integrated_sha || origin.candidate_tree !== cap.integrated_tree) throw new Error('bootstrap integration lacks exact sealed candidate origin')
    const required = BOOTSTRAP_INTEGRATION_GATES[origin.operation_id]
    if (!required) throw new Error('bootstrap integration candidate operation is not finalizable')
    const receipts = Array.isArray(cap.gate_receipts) ? cap.gate_receipts : []
    if (receipts.length !== required.length || new Set(receipts.map(item => item.gate_id)).size !== required.length) throw new Error('bootstrap integration gate receipt set is incomplete or duplicated')
    for (const gateId of required) {
      const receipt = receipts.find(item => item.gate_id === gateId)
      if (!receipt || receipt.exit_code !== 0 || receipt.tested_sha !== cap.integrated_sha || receipt.tested_tree !== cap.integrated_tree ||
          receipt.receipt_id !== brokerGateReceiptId(cap.operation_id, { ...receipt, raw_output_sha256: receipt.broker_evidence?.raw_output_sha256 }) || receipt.broker_evidence?.logical_operation !== `gate:${gateId}` ||
          !samePath(receipt.broker_evidence?.physical_cwd || '', cap.worktree)) throw new Error(`bootstrap integration gate receipt is not bound: ${gateId}`)
    }
    return receipts
  }

  #assertLanePath(cap, relPath, allowMissing = false) {
    const rel = normalizeRel(relPath)
    if (!this.#allowed(cap, rel)) throw new Error(`path is outside capability scope: ${rel}`)
    return { rel, absolute: assertNoReparse(cap.worktree, join(cap.worktree, ...rel.split('/')), allowMissing) }
  }

  #changedPaths(cap) {
    const unstaged = this.#git(['diff', '--name-only', '-z'], cap.worktree).stdout.split('\0').filter(Boolean)
    const staged = this.#git(['diff', '--cached', '--name-only', '-z'], cap.worktree).stdout.split('\0').filter(Boolean)
    const untracked = this.#git(['ls-files', '--others', '--exclude-standard', '-z'], cap.worktree).stdout.split('\0').filter(path => path && path !== '.atx-lease')
    return [...new Set([...unstaged, ...staged, ...untracked].map(normalizeRel))].sort()
  }

  #assertLaneChanges(cap, requireChanges = false) {
    const paths = this.#changedPaths(cap)
    if (requireChanges && !paths.length) throw new Error('lane has no source changes')
    for (const path of paths) if (!this.#allowed(cap, path)) throw new Error(`lane mutation escaped capability scope: ${path}`)
    return paths
  }

  #protectedIgnoredIntegrity(cap) {
    const protectedPaths = ['build/build.ninja']
    return Object.fromEntries(protectedPaths.map(rel => {
      const absolute = join(cap.worktree, ...rel.split('/'))
      if (!existsSync(absolute)) return [rel, null]
      assertNoReparse(cap.worktree, absolute)
      return [rel, sha256(readFileSync(absolute))]
    }))
  }

  refResolve(input) {
    assertExactKeys(input, ['ref_id'])
    const ref = input.ref_id === 'main' ? MAIN_REF : input.ref_id === 'canonical' ? CANONICAL_REF : String(input.ref_id || '')
    if (![MAIN_REF, CANONICAL_REF].includes(ref) && !SHA_RE.test(ref)) throw new Error('ref_id must be main, canonical, or an exact commit SHA')
    const before = this.rootGuard()
    const result = processResult('git', ['rev-parse', '--verify', `${ref}^{commit}`], this.root)
    const after = this.rootGuard()
    this.#assertGuard(before, after)
    const sha = result.exit_code === 0 ? result.stdout.trim().toLowerCase() : 'MISSING'
    return { ref: ref === MAIN_REF ? 'main' : ref === CANONICAL_REF ? CANONICAL_REF : ref, sha, evidence: this.#evidence('ref_resolve', this.root, `git rev-parse --verify ${ref}^{commit}`, result, before, after) }
  }

  capabilityProbe() {
    const before = this.rootGuard()
    const script = join(this.root, 'scripts', 'oracle-capability.ps1')
    const result = processResult('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', script], this.root)
    const after = this.rootGuard()
    this.#assertGuard(before, after)
    requireSuccess(result, 'capability probe')
    const value = JSON.parse(result.stdout.trim())
    return { ...value, broker_evidence: this.#evidence('capability_probe', this.root, 'powershell scripts\\oracle-capability.ps1', result, before, after) }
  }

  openLane(input) {
    assertExactKeys(input, ['operation_id', 'stage', 'run_id', 'branch', 'base_sha', 'heartbeat_id', 'scope_paths'], ['operation_id', 'stage', 'run_id', 'branch', 'base_sha', 'heartbeat_id'])
    const spec = OPERATION_REGISTRY[input.operation_id]
    if (!spec || input.stage !== spec.stage) throw new Error('unknown operation/stage pair')
    if (!SAFE_ID_RE.test(input.run_id) || !SAFE_ID_RE.test(input.heartbeat_id) || !SAFE_BRANCH_RE.test(input.branch) || !spec.branch.test(input.branch)) throw new Error('operation identity is not registry-conforming')
    const baseSha = String(input.base_sha || '').toLowerCase()
    if (!SHA_RE.test(baseSha) || this.#ref(baseSha) !== baseSha) throw new Error('base_sha must be an existing exact commit')
    const scope = Array.isArray(input.scope_paths) ? [...new Set(input.scope_paths.map(normalizeRel))].sort() : []
    if (input.operation_id !== 'sprint_build' && scope.length) throw new Error('scope_paths is valid only for sprint_build')
    const provisional = { operation_id: input.operation_id, scope_paths: scope }
    for (const item of scope) if (!this.#allowed(provisional, item)) throw new Error(`requested scope is outside the fixed operation registry: ${item}`)
    if (input.operation_id === 'bootstrap_integration') {
      const canonical = this.#ref(CANONICAL_REF)
      if (canonical !== null && canonical !== baseSha) throw new Error('bootstrap integration base must equal the current canonical ref')
    }
    const recoveryJournal = input.operation_id === 'bootstrap_data' ? this.#loadRecovery({ ...input, base_sha: baseSha }) : null
    if (recoveryJournal) this.#assertRecoveryJournal(recoveryJournal)
    const leaseStartSha = recoveryJournal ? recoveryJournal.result.sha : baseSha
    const stage2Predecessors = []
    for (const entry of existsSync(this.poolRoot) ? readdirSync(this.poolRoot, { withFileTypes: true }) : []) {
      if (!entry.isDirectory() || !/^pool-[0-9]+$/.test(entry.name)) continue
      const marker = join(this.poolRoot, entry.name, '.atx-quarantine-v3')
      if (!existsSync(marker)) continue
      const record = parseRecord(readFileSync(marker, 'ascii'))
      if (record.run_id === input.run_id && record.branch === input.branch && record.heartbeat_id === input.heartbeat_id) {
        throw new Error(`deterministic lane is quarantined for audit: ${entry.name}`)
      }
    }
    for (const file of existsSync(this.capDir) ? readdirSync(this.capDir) : []) {
      if (!/^[0-9a-f]{64}\.json$/.test(file)) continue
      const token = file.slice(0, -5)
      const existing = this.#loadCap(token, false)
      if (existing.token_hash !== sha256(token)) throw new Error('durable capability filename/token binding mismatch')
      const exactIdentity = existing.operation_id === input.operation_id && existing.stage === input.stage && existing.run_id === input.run_id && existing.branch === input.branch &&
        existing.base_sha === baseSha && existing.heartbeat_id === input.heartbeat_id && JSON.stringify(existing.scope_paths || []) === JSON.stringify(scope)
      const conflictingStage2Identity = input.operation_id === 'bootstrap_mode_a' && existing.operation_id === 'bootstrap_mode_a' && existing.stage === 'bootstrap-2' &&
        (existing.run_id === input.run_id || existing.branch === input.branch)
      if (conflictingStage2Identity && !exactIdentity) throw new Error('deterministic Stage 2 identity conflicts with durable capability history')
      if (exactIdentity) {
        if (existing.state === 'active') {
          this.#revalidateLease(existing)
          const guard = this.rootGuard()
          const idempotent = { exit_code: 0, output: existing.acquisition_output, stdout: existing.acquisition_output, stderr: '' }
          return this.#acquireResponse(existing, token, idempotent, guard, guard)
        }
        if (existing.state === 'released' && recoveryJournal && existing.released_head === recoveryJournal.result.sha && existing.released_tree === recoveryJournal.result.tree) continue
        if (existing.state === 'released' && input.operation_id === 'bootstrap_mode_a') {
          const baseTree = this.#tree(baseSha)
          const branchSha = this.#ref(`refs/heads/${input.branch}`)
          if (existing.lease_start_sha !== baseSha || existing.released_head !== baseSha || existing.released_tree !== baseTree || existing.canonical_before !== baseSha ||
              existing.recovery_id || existing.recovery_sha || existing.recovery_tree || existing.stage1_gate_failure || branchSha !== baseSha) {
            throw new Error('released deterministic Stage 2 lane failed exact clean-base reopen audit')
          }
          stage2Predecessors.push({ capability_hash: existing.token_hash, released_head: existing.released_head, released_tree: existing.released_tree, lease_name: existing.lease_name })
          continue
        }
        throw new Error(`deterministic lane capability is incompatible ${existing.state}: ${existing.lease_name}`)
      }
    }
    const before = this.rootGuard()
    const script = join(this.root, 'scripts', 'lease-worktree.ps1')
    const args = ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', script, '-Branch', input.branch, '-Base', leaseStartSha, '-Agent', 'vol-oracle-broker-v3', '-RunId', input.run_id, '-HeartbeatId', input.heartbeat_id, '-MaxPool', '20']
    const result = processResult('powershell', args, this.root)
    const after = this.rootGuard()
    this.#assertGuard(before, after)
    requireSuccess(result, 'broker lane acquisition')
    const match = result.output.match(/LEASED pool=(pool-[0-9]+) path=([^\r\n]+?) branch=([^\s]+) base_sha=([0-9a-f]{40}) run_id=([^\s]+) owner_kind=heartbeat keeper_pid=([0-9]+) keeper_started_utc=([^\s]+) keeper_ready_utc=([^\s]+)/i)
    if (!match) throw new Error('lease output did not contain a typed keeper acquisition')
    const leaseName = match[1]
    const { lane, record } = this.#leasePath(leaseName)
    if (!samePath(match[2], lane) || match[3] !== input.branch || lower(match[4]) !== leaseStartSha || match[5] !== input.run_id) throw new Error('lease output identity mismatch')
    const lease = parseRecord(readFileSync(record, 'ascii'))
    let capabilityGuardAfter = after
    if (input.operation_id === 'bootstrap_mode_a' && stage2Predecessors.length) {
      let acquiredChanges = null
      let auditFailure = null
      try {
        const expectedTree = this.#tree(baseSha)
        const acquiredHead = this.#git(['rev-parse', 'HEAD'], lane).stdout.trim().toLowerCase()
        const acquiredTree = this.#tree(acquiredHead, lane)
        acquiredChanges = this.#changedPaths({ worktree: lane })
        capabilityGuardAfter = this.rootGuard()
        this.#assertGuard(after, capabilityGuardAfter)
        if (acquiredHead !== baseSha || acquiredTree !== expectedTree || acquiredChanges.length) auditFailure = new Error('post-acquisition HEAD/tree/clean mismatch')
      } catch (error) {
        auditFailure = error
      }
      if (auditFailure) {
        const preserveLane = !Array.isArray(acquiredChanges) || acquiredChanges.length > 0
        const cleanupArgs = preserveLane
          ? ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', script, '-StopKeeper', leaseName, '-RunId', input.run_id]
          : ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', script, '-Release', leaseName, '-RunId', input.run_id]
        const cleanupBefore = this.rootGuard()
        const cleanup = processResult('powershell', cleanupArgs, this.root)
        const cleanupAfter = this.rootGuard()
        this.#assertGuard(cleanupBefore, cleanupAfter)
        requireSuccess(cleanup, 'rejected Stage 2 reopen cleanup')
        const disposition = preserveLane ? 'dirty lane preserved and keeper stopped' : 'clean lane released'
        throw new Error(`reopened Stage 2 lane failed post-acquisition base/clean audit; ${disposition}`)
      }
    }
    const token = randomBytes(32).toString('hex')
    const cap = {
      version: VERSION, state: 'active', token_hash: sha256(token), operation_id: input.operation_id, stage: input.stage,
      run_id: input.run_id, branch: input.branch, base_sha: baseSha, heartbeat_id: input.heartbeat_id,
      lease_start_sha: leaseStartSha, recovery_replay: !!recoveryJournal,
      lease_name: leaseName, worktree: lane, lease_token: lease.lease_token, keeper_pid: Number(lease.keeper_pid),
      keeper_process_started_utc: lease.keeper_process_started_utc, keeper_ready_utc: lease.keeper_ready_utc,
      canonical_before: before.canonical_sha, scope_paths: scope, acquisition_output: result.output,
      acquisition_root_guard_before: before, acquisition_root_guard_after: capabilityGuardAfter,
      ...(input.operation_id === 'bootstrap_mode_a' ? { attempt_epoch: stage2Predecessors.length + 1, reopen_predecessors: stage2Predecessors.sort((a, b) => a.capability_hash.localeCompare(b.capability_hash)) } : {}),
    }
    this.#saveCap(token, cap)
    this.#revalidateLease(cap)
    return this.#acquireResponse(cap, token, result, before, after)
  }

  #acquireResponse(cap, token, result, before, after) {
    return {
      capability: token, operation_id: cap.operation_id, stage: cap.stage, lease_name: cap.lease_name, run_id: cap.run_id,
      branch: cap.branch, base_sha: cap.base_sha, lease_start_sha: cap.lease_start_sha || cap.base_sha, recovery_replay: !!cap.recovery_replay, worktree: cap.worktree, heartbeat_id: cap.heartbeat_id,
      keeper_pid: cap.keeper_pid, keeper_process_started_utc: cap.keeper_process_started_utc, keeper_ready_utc: cap.keeper_ready_utc,
      acquisition_receipt: { action: 'acquire', lease_name: cap.lease_name, run_id: cap.run_id, branch: cap.branch, base_sha: cap.base_sha, worktree: cap.worktree, heartbeat_id: cap.heartbeat_id, keeper_pid: cap.keeper_pid, keeper_process_started_utc: cap.keeper_process_started_utc, keeper_ready_utc: cap.keeper_ready_utc, exit_code: 0, output: result.output },
      broker_evidence: this.#evidence('lane_open', this.root, `lease-worktree:${cap.operation_id}`, result, before, after),
    }
  }

  listWorkspace(input) {
    assertExactKeys(input, ['capability'])
    const cap = this.#loadCap(input.capability)
    const paths = this.#git(['ls-files', '-z'], cap.worktree).stdout.split('\0').filter(Boolean).map(normalizeRel).filter(path => this.#allowed(cap, path))
    return { capability: input.capability, files: paths.map(path => ({ path, file_id: this.#fileId(path), sha256: sha256(readFileSync(join(cap.worktree, ...path.split('/')))) })) }
  }

  #repoFiles() {
    const result = this.#git(['ls-files', '-z', '--', 'atx-vol', '.claude', 'scripts', 'docs'], this.root)
    return result.stdout.split('\0').filter(Boolean).map(normalizeRel).filter(path => !/\/cohorts\/(?:holdout|tune|smoke)\.json$/i.test(path) && !/\.(?:parquet|zip)$/i.test(path))
  }

  #fileId(relPath) {
    return createHmac('sha256', this.secret).update(`file-v3\0${normalizeRel(relPath)}`).digest('hex')
  }

  repoSearch(input) {
    assertExactKeys(input, ['query'])
    const query = String(input.query || '')
    if (!query || query.length > 160 || /[\r\n\0]/.test(query)) throw new Error('query must be a short literal string')
    const before = this.rootGuard()
    const hits = []
    for (const path of this.#repoFiles()) {
      const absolute = assertNoReparse(this.root, join(this.root, ...path.split('/')))
      if (statSync(absolute).size > 1024 * 1024) continue
      const text = readFileSync(absolute, 'utf8')
      let start = 0
      while (hits.length < 80) {
        const index = text.indexOf(query, start)
        if (index < 0) break
        const line = text.slice(0, index).split('\n').length
        hits.push({ path, file_id: this.#fileId(path), line, excerpt: text.slice(Math.max(0, index - 80), Math.min(text.length, index + query.length + 160)).replace(/[\r\n]+/g, ' ') })
        start = index + Math.max(1, query.length)
      }
      if (hits.length >= 80) break
    }
    const after = this.rootGuard()
    this.#assertGuard(before, after)
    return { query, hits, root_guard: after }
  }

  repoRead(input) {
    assertExactKeys(input, ['file_id'])
    const file = this.#repoFiles().find(path => this.#fileId(path) === input.file_id)
    if (!file) throw new Error('unknown repository artifact id')
    const absolute = assertNoReparse(this.root, join(this.root, ...file.split('/')))
    if (statSync(absolute).size > 512 * 1024) throw new Error('artifact exceeds reader limit')
    const before = this.rootGuard()
    const content = readFileSync(absolute, 'utf8')
    const after = this.rootGuard()
    this.#assertGuard(before, after)
    return { path: file, file_id: input.file_id, content, sha256: sha256(Buffer.from(content)), root_guard: after }
  }

  applyPatch(input) {
    assertExactKeys(input, ['capability', 'patch'])
    const cap = this.#loadCap(input.capability)
    if (['bootstrap_data', 'bootstrap_integration', 'sprint_integration'].includes(cap.operation_id)) throw new Error(`patch_apply is forbidden for ${cap.operation_id}`)
    const patch = String(input.patch || '')
    if (!patch || Buffer.byteLength(patch) > 2 * 1024 * 1024 || patch.includes('\0')) throw new Error('patch is empty or too large')
    if (/^GIT binary patch$/m.test(patch) || /^rename (?:from|to) /m.test(patch)) throw new Error('binary and rename patches are forbidden')
    const paths = []
    const starts = [...patch.matchAll(/^diff --git /gm)].map(match => match.index)
    if (!starts.length || patch.slice(0, starts[0]).trim()) throw new Error('patch has content outside canonical diff sections')
    starts.push(patch.length)
    for (let index = 0; index < starts.length - 1; index += 1) {
      const section = patch.slice(starts[index], starts[index + 1])
      const lines = section.split(/\r?\n/)
      const match = lines[0].match(/^diff --git a\/(.+) b\/(.+)$/)
      if (!match) throw new Error('patch diff header is not canonical')
      const left = normalizeRel(match[1]); const right = normalizeRel(match[2])
      if (left !== right) throw new Error('rename/path substitution is forbidden')
      const oldHeaders = lines.filter(line => line.startsWith('--- '))
      const newHeaders = lines.filter(line => line.startsWith('+++ '))
      if (oldHeaders.length !== 1 || newHeaders.length !== 1) throw new Error('patch must contain one exact ---/+++ pair per diff')
      const isCreate = lines.some(line => /^new file mode 100[0-7]{3}$/.test(line))
      const expectedOld = isCreate ? '/dev/null' : `a/${left}`
      const expectedNew = `b/${right}`
      if (oldHeaders[0] !== `--- ${expectedOld}` || newHeaders[0] !== `+++ ${expectedNew}`) throw new Error('patch ---/+++ path differs from diff header')
      if (!isCreate && lines.some(line => /^deleted file mode /i.test(line))) throw new Error('raw patch deletion is forbidden')
      paths.push(right)
    }
    for (const path of [...new Set(paths)]) this.#assertLanePath(cap, path, true)
    const protectedBefore = this.#protectedIgnoredIntegrity(cap)
    const before = this.rootGuard()
    const check = processResult('git', ['apply', '--index', '--check', '--whitespace=nowarn', '-'], cap.worktree, patch)
    requireSuccess(check, 'patch preflight')
    const apply = processResult('git', ['apply', '--index', '--whitespace=nowarn', '-'], cap.worktree, patch)
    const after = this.rootGuard()
    this.#assertGuard(before, after)
    requireSuccess(apply, 'patch apply')
    if (JSON.stringify(this.#protectedIgnoredIntegrity(cap)) !== JSON.stringify(protectedBefore)) throw new Error('protected ignored/build integrity changed')
    const changed = this.#assertLaneChanges(cap, true)
    return { changed_paths: changed, broker_evidence: this.#evidence('patch_apply', cap.worktree, 'git apply <validated scoped patch>', apply, before, after) }
  }

  runGate(input) {
    assertExactKeys(input, ['capability', 'gate_id'])
    const cap = this.#loadCap(input.capability)
    if (cap.operation_id === 'bootstrap_data') throw new Error('gate_run is forbidden for bootstrap_data; use recover_stage1')
    return this.#runGate(cap, input.gate_id, input.capability)
  }

  #runGate(cap, gateId, capabilityToken = null) {
    const gate = this.gates[gateId]
    if (!gate) throw new Error('gate_id is not in the fixed broker registry')
    if (['holdout_mode_a', 'holdout_mode_b'].includes(gateId) && cap.operation_id !== 'ratchet') throw new Error('holdout gate is restricted to Ratchet')
    if (gateId === 'rel_avx2_speed' && cap.operation_id !== 'ratchet') throw new Error('Ratchet speed gate used outside Ratchet')
    if (gateId.startsWith('measure_') && cap.operation_id !== 'measure') throw new Error('Measure gate used outside Measure')
    const integrationOperation = ['bootstrap_integration', 'sprint_integration'].includes(cap.operation_id)
    if (integrationOperation) {
      if (!cap.integrated_sha || !cap.integrated_tree) throw new Error('integration gates require one sealed lane_integrate result')
      if (cap.operation_id === 'bootstrap_integration') {
        this.#assertBootstrapCanonical(cap)
        const required = BOOTSTRAP_INTEGRATION_GATES[cap.bootstrap_origin?.operation_id] || []
        if (!required.includes(gateId)) throw new Error('gate_id is not required for the reviewed bootstrap operation/stage')
        if ((cap.gate_receipts || []).some(receipt => receipt.gate_id === gateId)) throw new Error('bootstrap integration gate receipt already exists')
      }
      const currentHead = this.#git(['rev-parse', 'HEAD'], cap.worktree).stdout.trim().toLowerCase()
      if (currentHead !== cap.integrated_sha || this.#tree(currentHead, cap.worktree) !== cap.integrated_tree) throw new Error('integration HEAD/tree differs from sealed reviewed candidate')
      if (this.#changedPaths(cap).length) throw new Error('integration lane changed after sealed integration')
    }
    const before = this.rootGuard()
    const result = gate.file
      ? processResult('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', join(cap.worktree, ...gate.file.split('/')), ...gate.args], cap.worktree)
      : processResult(gate.exe, gate.args, cap.worktree)
    const after = this.rootGuard()
    this.#assertGuard(before, after)
    const changed = this.#assertLaneChanges(cap, false)
    const testedSha = this.#git(['rev-parse', 'HEAD'], cap.worktree).stdout.trim().toLowerCase()
    const testedTree = this.#tree(testedSha, cap.worktree)
    if (integrationOperation && (changed.length || testedSha !== cap.integrated_sha || testedTree !== cap.integrated_tree)) throw new Error('gate changed sealed integration SHA/tree or worktree')
    const brokerEvidence = this.#evidence(`gate:${gateId}`, cap.worktree, gate.display, result, before, after)
    // Gate evidence is committed to the exact UTF-8 bytes of the canonical carried output.
    // Unlike process stdout/stderr framing, this value is independently recomputable by a workflow consumer.
    const receipt = buildBrokerGateReceipt(cap.operation_id, {
      gate_id: gateId, tested_sha: testedSha, tested_tree: testedTree, command: gate.display, exit_code: result.exit_code,
      output: result.output, broker_evidence: brokerEvidence,
    })
    if (cap.operation_id === 'bootstrap_integration') this.#saveCap(capabilityToken, { ...cap, gate_receipts: [...(cap.gate_receipts || []), receipt] })
    return receipt
  }

  commitLane(input) {
    assertExactKeys(input, ['capability', 'message_id'])
    const cap = this.#loadCap(input.capability)
    if (['bootstrap_data', 'bootstrap_integration', 'sprint_integration'].includes(cap.operation_id)) throw new Error(`lane_commit is forbidden for ${cap.operation_id}`)
    return this.#commitLane(cap, input.message_id)
  }

  #commitLane(cap, messageId) {
    const message = MESSAGE_REGISTRY[messageId]
    if (!message) throw new Error('message_id is not registered')
    const paths = this.#assertLaneChanges(cap, true)
    const before = this.rootGuard()
    const add = processResult('git', ['add', '--', ...paths], cap.worktree)
    requireSuccess(add, 'scoped lane add')
    const staged = this.#git(['diff', '--cached', '--name-only', '-z'], cap.worktree).stdout.split('\0').filter(Boolean).map(normalizeRel).sort()
    if (JSON.stringify(staged) !== JSON.stringify(paths)) throw new Error('staged paths differ from validated lane paths')
    const commit = processResult('git', ['commit', '-m', message, '--', ...paths], cap.worktree)
    const after = this.rootGuard()
    this.#assertGuard(before, after)
    requireSuccess(commit, 'lane commit')
    const sha = this.#git(['rev-parse', 'HEAD'], cap.worktree).stdout.trim().toLowerCase()
    if (this.#changedPaths(cap).length) throw new Error('lane remains dirty after scoped commit')
    const fileBlobOids = Object.fromEntries(paths.map(path => {
      const blob = processResult('git', ['rev-parse', '--verify', `${sha}:${path}`], cap.worktree)
      return [path, blob.exit_code === 0 && SHA_RE.test(blob.stdout.trim().toLowerCase())
        ? blob.stdout.trim().toLowerCase() : null]
    }))
    return { sha, tree: this.#tree(sha, cap.worktree), files_changed: paths,
      file_blob_oids: fileBlobOids,
      broker_evidence: this.#evidence('lane_commit', cap.worktree, `git commit -- <${paths.length} validated paths>`, commit, before, after) }
  }

  inspectCommit(input) {
    assertExactKeys(input, ['base_sha', 'candidate_sha'])
    const base = String(input.base_sha || '').toLowerCase(); const candidate = String(input.candidate_sha || '').toLowerCase()
    if (!SHA_RE.test(base) || !SHA_RE.test(candidate) || !this.#ref(base) || !this.#ref(candidate)) throw new Error('commit inspection requires exact existing SHAs')
    const before = this.rootGuard()
    const pathsResult = processResult('git', ['diff', '--name-only', '-z', `${base}...${candidate}`], this.root)
    requireSuccess(pathsResult, 'commit path inspection')
    const paths = pathsResult.stdout.split('\0').filter(Boolean).map(normalizeRel)
    if (paths.some(path => /\/cohorts\/(?:holdout|tune|smoke)\.json$/i.test(path))) throw new Error('cohort membership diff is not review-readable')
    const diff = processResult('git', ['diff', '--no-ext-diff', '--unified=40', `${base}...${candidate}`, '--', ...paths], this.root)
    const after = this.rootGuard()
    this.#assertGuard(before, after)
    requireSuccess(diff, 'commit diff inspection')
    if (Buffer.byteLength(diff.output) > 2 * 1024 * 1024) throw new Error('review diff exceeds broker limit')
    return { base_sha: base, candidate_sha: candidate, paths, diff: diff.output, broker_evidence: this.#evidence('commit_inspect', this.root, `git diff ${base}...${candidate} -- <validated paths>`, diff, before, after) }
  }

  integrate(input) {
    assertExactKeys(input, ['capability', 'reviewed_shas'])
    const cap = this.#loadCap(input.capability)
    if (!['bootstrap_integration', 'sprint_integration'].includes(cap.operation_id)) throw new Error('capability cannot integrate commits')
    if (cap.integrated_sha || cap.integrated_tree) throw new Error('integration capability already sealed')
    if (!Array.isArray(input.reviewed_shas) || input.reviewed_shas.length < 1 || input.reviewed_shas.length > 4) throw new Error('reviewed_shas must contain 1-4 commits')
    const shas = input.reviewed_shas.map(value => String(value).toLowerCase())
    if (new Set(shas).size !== shas.length || shas.some(sha => !SHA_RE.test(sha) || !this.#ref(sha))) throw new Error('reviewed_shas must be unique existing exact commits')
    if (cap.operation_id === 'bootstrap_integration') {
      this.#assertBootstrapCanonical(cap)
      if (shas.length !== 1) throw new Error('bootstrap integration requires exactly one reviewed candidate')
    }
    if (this.#changedPaths(cap).length) throw new Error('integration lane must start clean')
    const receipts = []
    const reviewedCandidates = []
    let bootstrapOrigin = null
    for (const sha of shas) {
      const reviewedTree = this.#tree(sha, cap.worktree)
      if (cap.operation_id === 'bootstrap_integration') bootstrapOrigin = this.#bootstrapCandidateOrigin(cap, sha, reviewedTree)
      reviewedCandidates.push({ sha, tree: reviewedTree })
      const ancestor = processResult('git', ['merge-base', '--is-ancestor', cap.base_sha, sha], cap.worktree)
      if (ancestor.exit_code !== 0) throw new Error(`reviewed commit is not based on frozen base: ${sha}`)
      const diffPaths = this.#git(['diff', '--name-only', '-z', `${cap.base_sha}...${sha}`], cap.worktree).stdout.split('\0').filter(Boolean).map(normalizeRel)
      for (const path of diffPaths) if (!this.#allowed(cap, path)) throw new Error(`integration commit changes forbidden path: ${path}`)
      const before = this.rootGuard()
      let result
      if (cap.operation_id === 'bootstrap_integration') {
        result = processResult('git', ['merge', '--ff-only', sha], cap.worktree)
      } else {
        const preflight = processResult('git', ['merge-tree', '--write-tree', 'HEAD', sha], cap.worktree)
        requireSuccess(preflight, 'integration merge preflight')
        result = processResult('git', ['merge', '--no-ff', '-m', MESSAGE_REGISTRY.sprint_integration, sha], cap.worktree)
      }
      const after = this.rootGuard()
      this.#assertGuard(before, after)
      requireSuccess(result, 'exact reviewed commit integration')
      const head = this.#git(['rev-parse', 'HEAD'], cap.worktree).stdout.trim().toLowerCase()
      receipts.push({ reviewed_sha: sha, reviewed_tree: reviewedTree, head_after: head, tree_after: this.#tree(head, cap.worktree), command: cap.operation_id === 'bootstrap_integration' ? `git merge --ff-only ${sha}` : `git merge --no-ff ${sha}`, exit_code: 0, output: `${result.output}\n${head}`.trim(), broker_evidence: this.#evidence('lane_integrate', cap.worktree, `integrate-reviewed:${sha}`, result, before, after) })
    }
    if (this.#changedPaths(cap).length) throw new Error('integration left the lane dirty')
    const beforeHead = this.rootGuard()
    const headResult = processResult('git', ['rev-parse', 'HEAD'], cap.worktree)
    const afterHead = this.rootGuard()
    this.#assertGuard(beforeHead, afterHead)
    requireSuccess(headResult, 'integration HEAD audit')
    const head = headResult.stdout.trim().toLowerCase()
    const tree = this.#tree(head, cap.worktree)
    this.#saveCap(input.capability, {
      ...cap, integrated_sha: head, integrated_tree: tree, reviewed_candidates: reviewedCandidates,
      ...(bootstrapOrigin ? { bootstrap_origin: bootstrapOrigin, gate_receipts: [] } : {}),
    })
    return {
      integrated_shas: shas, reviewed_candidates: reviewedCandidates, sha: head, tree, integration_receipts: receipts,
      head_receipt: { ref: 'HEAD', sha: head, tree, command: 'git rev-parse HEAD', exit_code: 0, output: head, broker_evidence: this.#evidence('lane_head_audit', cap.worktree, 'git rev-parse HEAD', headResult, beforeHead, afterHead) },
    }
  }

  recoverStage1(input) {
    assertExactKeys(input, ['capability'])
    const cap = this.#loadCap(input.capability)
    if (cap.operation_id !== 'bootstrap_data' || cap.stage !== 'bootstrap-1') throw new Error('Stage 1 recovery requires bootstrap_data capability')
    if (!this.#isAncestor(this.recoverySource.parent, cap.base_sha)) throw new Error('Stage 1 recovery base must descend from the pinned source parent')
    const existingJournal = this.#loadRecovery(cap)
    if (existingJournal) return this.#replayRecovery(cap, existingJournal)
    if (this.#changedPaths(cap).length || this.#git(['rev-parse', 'HEAD'], cap.worktree).stdout.trim().toLowerCase() !== cap.base_sha) throw new Error('Stage 1 recovery lane is not pristine at its frozen base')
    const source = this.recoverySource
    if (this.#git(['cat-file', '-t', source.commit], this.root).stdout.trim() !== 'commit') throw new Error('Stage 1 recovery source is not a commit')
    const parent = this.#git(['show', '-s', '--format=%P', source.commit], this.root).stdout.trim().toLowerCase()
    const tree = this.#git(['show', '-s', '--format=%T', source.commit], this.root).stdout.trim().toLowerCase()
    if (parent !== source.parent || tree !== source.tree) throw new Error('Stage 1 recovery source parent/tree mismatch')
    const sourcePaths = this.#git(['diff-tree', '--no-commit-id', '--name-only', '-r', '-z', source.commit], this.root).stdout.split('\0').filter(Boolean).map(normalizeRel).sort()
    const expectedPaths = Object.keys(source.files).sort()
    if (JSON.stringify(sourcePaths) !== JSON.stringify(expectedPaths)) throw new Error('Stage 1 recovery source path set mismatch')
    const before = this.rootGuard()
    const blobProof = []
    for (const path of expectedPaths) {
      const blob = this.#git(['rev-parse', `${source.commit}:${path}`], this.root).stdout.trim().toLowerCase()
      if (blob !== source.files[path]) throw new Error(`Stage 1 recovery source blob mismatch: ${path}`)
      const content = this.#git(['show', `${source.commit}:${path}`], this.root).stdout
      const target = this.#assertLanePath(cap, path, true).absolute
      mkdirSync(dirname(target), { recursive: true })
      const temp = `${target}.${randomBytes(8).toString('hex')}.tmp`
      writeFileSync(temp, content, 'utf8')
      renameSync(temp, target)
      const replayBlob = this.#git(['hash-object', path], cap.worktree).stdout.trim().toLowerCase()
      if (replayBlob !== blob) throw new Error(`Stage 1 replay blob mismatch: ${path}`)
      blobProof.push({ path, source_blob: blob, replay_blob: replayBlob })
    }
    const replayPaths = this.#assertLaneChanges(cap, true)
    if (JSON.stringify(replayPaths) !== JSON.stringify(expectedPaths)) throw new Error('Stage 1 replay changed unexpected paths')
    const commitResult = this.#commitLane(cap, 'bootstrap_data_recovery')
    const changed = this.#git(['diff', '--name-only', '-z', `${cap.base_sha}...${commitResult.sha}`], cap.worktree).stdout.split('\0').filter(Boolean).map(normalizeRel).sort()
    const committedHead = this.#git(['rev-parse', 'HEAD'], cap.worktree).stdout.trim().toLowerCase()
    const committedTree = this.#tree(committedHead, cap.worktree)
    if (committedHead !== commitResult.sha || committedTree !== commitResult.tree || JSON.stringify(commitResult.files_changed) !== JSON.stringify(expectedPaths) ||
        JSON.stringify(changed) !== JSON.stringify(expectedPaths) || this.#changedPaths(cap).length) throw new Error('Stage 1 recovery committed HEAD/tree/path closure mismatch')
    const gateReceipts = ['aggregate_store', 'ingest_manifest', 'cohort_manifests', 'holdout_digest'].map(gate => this.#runGate(cap, gate))
    const postGateHead = this.#git(['rev-parse', 'HEAD'], cap.worktree).stdout.trim().toLowerCase()
    const postGateTree = this.#tree(postGateHead, cap.worktree)
    const gateFailure = gateReceipts.some(receipt => receipt.exit_code !== 0) || postGateHead !== commitResult.sha || postGateTree !== commitResult.tree || this.#changedPaths(cap).length
    if (gateFailure) {
      this.#saveCap(input.capability, { ...cap, stage1_gate_failure: { sha: commitResult.sha, tree: commitResult.tree, gate_receipts: gateReceipts } })
      const failedIds = gateReceipts.filter(receipt => receipt.exit_code !== 0).map(receipt => receipt.gate_id)
      throw new Error(`Stage 1 recovery fixed gate failed; quarantine required; failed_gate_ids=${failedIds.join(',') || 'lane_integrity'}`)
    }
    const after = this.rootGuard()
    this.#assertGuard(before, after)
    const recoveryId = this.#recoveryPath(cap).key
    const durableResult = {
      recovery_id: recoveryId, replayed: false,
      recovery: { source_commit: source.commit, source_parent: parent, source_tree: tree, blobs: blobProof, adoption_rerun: false },
      replay_paths: replayPaths, gate_receipts: gateReceipts, sha: commitResult.sha, tree: commitResult.tree, files_changed: commitResult.files_changed,
      changed_path_receipt: { base_sha: cap.base_sha, tested_sha: commitResult.sha, command: `git diff --name-only ${cap.base_sha}...${commitResult.sha}`, exit_code: 0, output: changed.join('\n'), paths: changed },
      broker_evidence: [this.#evidence('recover_stage1_source_replay', cap.worktree, 'recover-stage1:validated-source', { exit_code: 0, output: JSON.stringify({ parent, tree, paths: replayPaths }), stdout: JSON.stringify(blobProof), stderr: '' }, before, after), commitResult.broker_evidence],
    }
    const journal = this.#saveRecovery(cap, durableResult)
    this.#assertRecoveryJournal(journal)
    this.#saveCap(input.capability, { ...cap, recovery_id: recoveryId, recovery_sha: durableResult.sha, recovery_tree: durableResult.tree })
    return durableResult
  }

  #replayRecovery(cap, journal) {
    this.#assertRecoveryJournal(journal)
    const head = this.#git(['rev-parse', 'HEAD'], cap.worktree).stdout.trim().toLowerCase()
    const tree = this.#tree(head, cap.worktree)
    if (head !== journal.result.sha || tree !== journal.result.tree || this.#changedPaths(cap).length) throw new Error('active lane does not match sealed Stage 1 recovery SHA/tree')
    const before = this.rootGuard()
    const replay = { exit_code: 0, output: journal.recovery_id, stdout: journal.recovery_id, stderr: '' }
    const after = this.rootGuard()
    this.#assertGuard(before, after)
    return { ...journal.result, replayed: true, broker_evidence: [this.#evidence('recover_stage1_replay', cap.worktree, `recover-stage1:sealed-replay:${journal.recovery_id}`, replay, before, after)] }
  }

  recoveryResult(input) {
    assertExactKeys(input, ['capability'])
    const cap = this.#loadCap(input.capability)
    if (cap.operation_id !== 'bootstrap_data' || cap.stage !== 'bootstrap-1') throw new Error('recovery_result is restricted to Stage 1')
    const before = this.rootGuard()
    const journal = this.#loadRecovery(cap)
    if (!journal) {
      const result = { exit_code: 0, output: 'MISSING', stdout: 'MISSING', stderr: '' }
      const after = this.rootGuard(); this.#assertGuard(before, after)
      return { found: false, recovery_id: '', broker_evidence: this.#evidence('recovery_result', cap.worktree, 'recover-stage1:result-query', result, before, after) }
    }
    const replayed = this.#replayRecovery(cap, journal)
    const result = { exit_code: 0, output: journal.recovery_id, stdout: journal.recovery_id, stderr: '' }
    const after = this.rootGuard(); this.#assertGuard(before, after)
    return { found: true, recovery_id: journal.recovery_id, result: replayed, broker_evidence: this.#evidence('recovery_result', cap.worktree, 'recover-stage1:result-query', result, before, after) }
  }

  releaseLane(input) {
    assertExactKeys(input, ['capability'])
    const cap = this.#loadCap(input.capability)
    if (this.#changedPaths(cap).length) throw new Error('refusing to release a dirty broker lane')
    const head = this.#git(['rev-parse', 'HEAD'], cap.worktree).stdout.trim().toLowerCase()
    const tree = this.#tree(head, cap.worktree)
    let bootstrapExpectedOld = null
    let bootstrapGateReceipts = []
    if (cap.operation_id === 'bootstrap_data' && cap.stage1_gate_failure) {
      if (head !== cap.stage1_gate_failure.sha || tree !== cap.stage1_gate_failure.tree) throw new Error('failed Stage 1 lane differs from preserved committed SHA/tree')
      throw new Error('refusing to release failed Stage 1 gate lane; quarantine required')
    }
    if (['bootstrap_integration', 'sprint_integration'].includes(cap.operation_id)) {
      if (!cap.integrated_sha || !cap.integrated_tree || head !== cap.integrated_sha || tree !== cap.integrated_tree) throw new Error('integration release SHA/tree differs from sealed integration')
      if (!Array.isArray(cap.reviewed_candidates) || !cap.reviewed_candidates.length) throw new Error('integration release lacks sealed reviewed candidates')
      if (cap.operation_id === 'bootstrap_integration' && (cap.reviewed_candidates.length !== 1 || cap.reviewed_candidates[0].sha !== head || cap.reviewed_candidates[0].tree !== tree)) throw new Error('bootstrap integration is not the exact reviewed SHA/tree')
      if (cap.operation_id === 'bootstrap_integration') {
        bootstrapExpectedOld = this.#assertBootstrapCanonical(cap)
        bootstrapGateReceipts = this.#assertBootstrapGateSet(cap)
        if (!this.#isAncestor(cap.base_sha, head)) throw new Error('bootstrap integration target is not a descendant of its canonical base')
      }
    }
    const before = this.rootGuard()
    if (cap.operation_id === 'bootstrap_integration' && (before.canonical_sha || ZERO_SHA) !== bootstrapExpectedOld) throw new Error('bootstrap canonical drifted before integration release')
    const script = join(this.root, 'scripts', 'lease-worktree.ps1')
    const result = processResult('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', script, '-Release', cap.lease_name, '-RunId', cap.run_id], this.root)
    const after = this.rootGuard()
    this.#assertGuard(before, after)
    requireSuccess(result, 'broker lane release')
    let finalizeToken = ''
    if (OPERATION_REGISTRY[cap.operation_id].finalize) {
      if (cap.operation_id === 'ratchet' && (!cap.canonical_before || cap.canonical_before === head)) throw new Error('Ratchet finalize identity is invalid')
      finalizeToken = randomBytes(32).toString('hex')
      const finalSha = cap.operation_id === 'bootstrap_integration' ? cap.integrated_sha : head
      const finalTree = cap.operation_id === 'bootstrap_integration' ? cap.integrated_tree : tree
      this.#saveFinalize(finalizeToken, {
        version: VERSION, state: 'active', source_capability_hash: sha256(input.capability),
        expected_old_sha: cap.operation_id === 'bootstrap_integration' ? bootstrapExpectedOld : (cap.canonical_before || ZERO_SHA),
        new_sha: finalSha, new_tree: finalTree, operation_id: cap.operation_id, stage: cap.stage, branch: cap.branch, run_id: cap.run_id,
        ...(cap.operation_id === 'bootstrap_integration' ? {
          base_sha: cap.base_sha, canonical_before: cap.canonical_before, bootstrap_origin: cap.bootstrap_origin,
          gate_receipt_ids: bootstrapGateReceipts.map(receipt => receipt.receipt_id),
        } : {}),
      })
    }
    this.#saveCap(input.capability, { ...cap, state: 'released', released_head: head, released_tree: tree })
    return {
      released: true, lease_name: cap.lease_name, sha: head, tree, finalize_capability: finalizeToken,
      release_receipt: { action: 'release', lease_name: cap.lease_name, run_id: cap.run_id, branch: cap.branch, base_sha: cap.base_sha, worktree: cap.worktree, heartbeat_id: cap.heartbeat_id, keeper_pid: cap.keeper_pid, keeper_process_started_utc: cap.keeper_process_started_utc, keeper_ready_utc: cap.keeper_ready_utc, exit_code: 0, output: result.output },
      broker_evidence: this.#evidence('lane_release', this.root, `lease-worktree:release:${cap.lease_name}`, result, before, after),
    }
  }

  quarantineLane(input) {
    assertExactKeys(input, ['capability'])
    const cap = this.#loadCap(input.capability, false)
    if (cap.state !== 'active') throw new Error('capability is stale or inactive')
    if (cap.operation_id !== 'bootstrap_data') throw new Error('lane_quarantine is restricted to Stage 1 recovery')
    this.#revalidateLease(cap, { requireLiveKeeper: false, requireCheckedOutBranch: false })
    const head = this.#git(['rev-parse', 'HEAD'], cap.worktree).stdout.trim().toLowerCase()
    const tree = this.#tree(head, cap.worktree)
    const preservedPaths = this.#changedPaths(cap)
    const before = this.rootGuard()
    const script = join(this.root, 'scripts', 'lease-worktree.ps1')
    const result = processResult('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', script, '-Quarantine', cap.lease_name, '-RunId', cap.run_id], this.root)
    const after = this.rootGuard()
    this.#assertGuard(before, after)
    requireSuccess(result, 'broker lane quarantine')
    this.#saveCap(input.capability, { ...cap, state: 'quarantined', quarantined_head: head, quarantined_tree: tree, preserved_paths: preservedPaths })
    return {
      quarantined: true, lease_name: cap.lease_name, sha: head, tree, preserved_paths: preservedPaths, gate_receipts: cap.stage1_gate_failure?.gate_receipts || [],
      quarantine_receipt: { action: 'quarantine', lease_name: cap.lease_name, run_id: cap.run_id, branch: cap.branch, base_sha: cap.base_sha, worktree: cap.worktree, heartbeat_id: cap.heartbeat_id, keeper_pid: cap.keeper_pid, keeper_process_started_utc: cap.keeper_process_started_utc, keeper_ready_utc: cap.keeper_ready_utc, exit_code: 0, output: result.output },
      broker_evidence: this.#evidence('lane_quarantine', this.root, `lease-worktree:quarantine:${cap.lease_name}`, result, before, after),
    }
  }

  canonicalFinalize(input) {
    assertExactKeys(input, ['finalize_capability', 'expected_sha', 'expected_tree'])
    const token = input.finalize_capability
    const finalizer = this.#loadFinalize(token)
    const expectedSha = String(input.expected_sha || '').toLowerCase()
    const expectedTree = String(input.expected_tree || '').toLowerCase()
    if (!SHA_RE.test(expectedSha) || !SHA_RE.test(expectedTree) || expectedSha !== finalizer.new_sha || expectedTree !== finalizer.new_tree) throw new Error('workflow expected SHA/tree differs from sealed integration')
    if (finalizer.operation_id === 'bootstrap_integration') {
      const origin = finalizer.bootstrap_origin
      const expectedOld = finalizer.canonical_before === null ? ZERO_SHA : finalizer.base_sha
      const requiredGates = BOOTSTRAP_INTEGRATION_GATES[origin?.operation_id]
      if (finalizer.stage !== 'bootstrap-prepare' || !SHA_RE.test(finalizer.base_sha || '') || finalizer.expected_old_sha !== expectedOld ||
          (finalizer.canonical_before !== null && finalizer.canonical_before !== finalizer.base_sha) || !requiredGates ||
          origin.candidate_sha !== finalizer.new_sha || origin.candidate_tree !== finalizer.new_tree ||
          !Array.isArray(finalizer.gate_receipt_ids) || finalizer.gate_receipt_ids.length !== requiredGates.length ||
          new Set(finalizer.gate_receipt_ids).size !== requiredGates.length || finalizer.gate_receipt_ids.some(id => !SHA256_RE.test(id)) ||
          !this.#isAncestor(finalizer.base_sha, finalizer.new_sha)) throw new Error('sealed bootstrap finalizer operation/stage/base/gate binding is invalid')
      if (finalizer.canonical_before === null && origin.operation_id !== 'bootstrap_data') throw new Error('sealed absent-canonical finalizer is not Stage 1')
      if (finalizer.canonical_before !== null && origin.operation_id === 'bootstrap_data') throw new Error('sealed existing-canonical finalizer has wrong bootstrap stage')
    }
    const before = this.rootGuard()
    const actualOld = before.canonical_sha || ZERO_SHA
    if (actualOld !== finalizer.expected_old_sha) throw new Error('canonical compare-and-swap precondition is stale')
    if (!SHA_RE.test(finalizer.new_sha) || !this.#ref(finalizer.new_sha) || this.#tree(finalizer.new_sha) !== finalizer.new_tree) throw new Error('finalize target SHA/tree is not exact')
    const result = processResult('git', ['update-ref', CANONICAL_REF, finalizer.new_sha, finalizer.expected_old_sha], this.root)
    const after = this.rootGuard()
    this.#assertGuard(before, after, true)
    requireSuccess(result, 'oracle canonical compare-and-swap')
    if (after.canonical_sha !== finalizer.new_sha) throw new Error('canonical audit disagrees after compare-and-swap')
    const target = this.#finalizePath(token)
    rmSync(target)
    return { ref: CANONICAL_REF, new_sha: finalizer.new_sha, new_tree: finalizer.new_tree, expected_old_sha: finalizer.expected_old_sha, command: `git update-ref ${CANONICAL_REF} ${finalizer.new_sha} ${finalizer.expected_old_sha}`, exit_code: 0, output: finalizer.new_sha, broker_evidence: this.#evidence('canonical_finalize', this.root, `canonical-cas:${finalizer.operation_id}`, result, before, after) }
  }

  canonicalAudit() {
    const before = this.rootGuard()
    const result = processResult('git', ['rev-parse', '--verify', `${CANONICAL_REF}^{commit}`], this.root)
    const after = this.rootGuard()
    this.#assertGuard(before, after)
    const sha = result.exit_code === 0 ? result.stdout.trim().toLowerCase() : 'MISSING'
    return { ref: CANONICAL_REF, sha, command: `git rev-parse ${CANONICAL_REF}`, exit_code: result.exit_code, output: result.exit_code === 0 ? sha : (result.output || 'MISSING'), broker_evidence: this.#evidence('canonical_audit', this.root, `git rev-parse ${CANONICAL_REF}`, result, before, after) }
  }
}

const TOOL_DEFINITIONS = Object.freeze([
  { name: 'capability_probe', description: 'Run the one fixed aggregate-only oracle capability probe under a canonical-root guard.', inputSchema: { type: 'object', additionalProperties: false, properties: {} } },
  { name: 'ref_resolve', description: 'Resolve only main, oracle canonical, or an exact commit SHA without mutation.', inputSchema: { type: 'object', additionalProperties: false, required: ['ref_id'], properties: { ref_id: { type: 'string' } } } },
  { name: 'repo_search', description: 'Literal search of tracked non-membership repository artifacts; returns opaque file IDs.', inputSchema: { type: 'object', additionalProperties: false, required: ['query'], properties: { query: { type: 'string', maxLength: 160 } } } },
  { name: 'repo_read', description: 'Read one broker-issued opaque repository artifact ID.', inputSchema: { type: 'object', additionalProperties: false, required: ['file_id'], properties: { file_id: { type: 'string', pattern: '^[0-9a-f]{64}$' } } } },
  { name: 'lane_open', description: 'Acquire one fixed-registry keeper-backed v3 lane and issue an opaque capability.', inputSchema: { type: 'object', additionalProperties: false, required: ['operation_id', 'stage', 'run_id', 'branch', 'base_sha', 'heartbeat_id'], properties: { operation_id: { type: 'string', enum: Object.keys(OPERATION_REGISTRY) }, stage: { type: 'string' }, run_id: { type: 'string' }, branch: { type: 'string' }, base_sha: { type: 'string', pattern: '^[0-9a-f]{40}$' }, heartbeat_id: { type: 'string' }, scope_paths: { type: 'array', items: { type: 'string' } } } } },
  { name: 'workspace_list', description: 'List only artifacts bound to an active lane capability.', inputSchema: { type: 'object', additionalProperties: false, required: ['capability'], properties: { capability: { type: 'string', pattern: '^[0-9a-f]{64}$' } } } },
  { name: 'patch_apply', description: 'Apply a unified text patch only after scope, containment, and reparse validation in the leased lane.', inputSchema: { type: 'object', additionalProperties: false, required: ['capability', 'patch'], properties: { capability: { type: 'string', pattern: '^[0-9a-f]{64}$' }, patch: { type: 'string', maxLength: 2097152 } } } },
  { name: 'gate_run', description: 'Run one fixed registered targeted gate in the capability-bound leased root.', inputSchema: { type: 'object', additionalProperties: false, required: ['capability', 'gate_id'], properties: { capability: { type: 'string', pattern: '^[0-9a-f]{64}$' }, gate_id: { type: 'string', enum: Object.keys(GATE_REGISTRY) } } } },
  { name: 'lane_commit', description: 'Commit exactly the validated scoped lane paths with a fixed message ID.', inputSchema: { type: 'object', additionalProperties: false, required: ['capability', 'message_id'], properties: { capability: { type: 'string', pattern: '^[0-9a-f]{64}$' }, message_id: { type: 'string', enum: Object.keys(MESSAGE_REGISTRY) } } } },
  { name: 'commit_inspect', description: 'Read an exact commit diff without exposing frozen cohort membership.', inputSchema: { type: 'object', additionalProperties: false, required: ['base_sha', 'candidate_sha'], properties: { base_sha: { type: 'string', pattern: '^[0-9a-f]{40}$' }, candidate_sha: { type: 'string', pattern: '^[0-9a-f]{40}$' } } } },
  { name: 'lane_integrate', description: 'Integrate 1-4 exact reviewed SHAs through the fixed broker merge path.', inputSchema: { type: 'object', additionalProperties: false, required: ['capability', 'reviewed_shas'], properties: { capability: { type: 'string', pattern: '^[0-9a-f]{64}$' }, reviewed_shas: { type: 'array', minItems: 1, maxItems: 4, items: { type: 'string', pattern: '^[0-9a-f]{40}$' } } } } },
  { name: 'recover_stage1', description: 'Validate and replay the pinned Stage 1 source commit, run four fixed gates, and commit without adoption rerun.', inputSchema: { type: 'object', additionalProperties: false, required: ['capability'], properties: { capability: { type: 'string', pattern: '^[0-9a-f]{64}$' } } } },
  { name: 'recovery_result', description: 'Query and replay only the sealed durable Stage 1 result bound to an active deterministic capability.', inputSchema: { type: 'object', additionalProperties: false, required: ['capability'], properties: { capability: { type: 'string', pattern: '^[0-9a-f]{64}$' } } } },
  { name: 'lane_release', description: 'Release only the capability-bound clean lease and optionally issue a bound finalize capability.', inputSchema: { type: 'object', additionalProperties: false, required: ['capability'], properties: { capability: { type: 'string', pattern: '^[0-9a-f]{64}$' } } } },
  { name: 'lane_quarantine', description: 'Stop and remove a failed Stage 1 lease while preserving its dirty branch/worktree for audit.', inputSchema: { type: 'object', additionalProperties: false, required: ['capability'], properties: { capability: { type: 'string', pattern: '^[0-9a-f]{64}$' } } } },
  { name: 'canonical_finalize', description: 'Consume a broker-issued finalize capability only when workflow expected SHA/tree matches the sealed integration.', inputSchema: { type: 'object', additionalProperties: false, required: ['finalize_capability', 'expected_sha', 'expected_tree'], properties: { finalize_capability: { type: 'string', pattern: '^[0-9a-f]{64}$' }, expected_sha: { type: 'string', pattern: '^[0-9a-f]{40}$' }, expected_tree: { type: 'string', pattern: '^[0-9a-f]{40}$' } } } },
  { name: 'canonical_audit', description: 'Read only refs/heads/oracle/canonical under the root guard.', inputSchema: { type: 'object', additionalProperties: false, properties: {} } },
])

function dispatch(broker, name, input) {
  const routes = {
    capability_probe: () => broker.capabilityProbe(), ref_resolve: () => broker.refResolve(input), repo_search: () => broker.repoSearch(input), repo_read: () => broker.repoRead(input),
    lane_open: () => broker.openLane(input), workspace_list: () => broker.listWorkspace(input), patch_apply: () => broker.applyPatch(input), gate_run: () => broker.runGate(input),
    lane_commit: () => broker.commitLane(input), commit_inspect: () => broker.inspectCommit(input), lane_integrate: () => broker.integrate(input), recover_stage1: () => broker.recoverStage1(input), recovery_result: () => broker.recoveryResult(input),
    lane_release: () => broker.releaseLane(input), lane_quarantine: () => broker.quarantineLane(input), canonical_finalize: () => broker.canonicalFinalize(input), canonical_audit: () => broker.canonicalAudit(),
  }
  if (!routes[name]) throw new Error(`unknown broker tool: ${name}`)
  return routes[name]()
}

async function serve() {
  const broker = new OracleLaneBroker()
  const lines = readline.createInterface({ input: process.stdin, crlfDelay: Infinity })
  for await (const line of lines) {
    if (!line.trim()) continue
    let request
    try {
      request = JSON.parse(line)
      if (request.method === 'notifications/initialized' || request.method === 'notifications/cancelled') continue
      let result
      if (request.method === 'initialize') result = { protocolVersion: request.params?.protocolVersion || '2025-06-18', capabilities: { tools: { listChanged: false } }, serverInfo: { name: 'oracle_lane_broker', version: '3.0.0' } }
      else if (request.method === 'ping') result = {}
      else if (request.method === 'tools/list') result = { tools: TOOL_DEFINITIONS }
      else if (request.method === 'tools/call') {
        const value = dispatch(broker, request.params?.name, request.params?.arguments || {})
        result = { content: [{ type: 'text', text: JSON.stringify(value) }], structuredContent: value, isError: false }
      } else throw new Error(`unsupported MCP method: ${request.method}`)
      process.stdout.write(`${JSON.stringify({ jsonrpc: '2.0', id: request.id, result })}\n`)
    } catch (error) {
      const message = String(error?.message || error)
      process.stdout.write(`${JSON.stringify({ jsonrpc: '2.0', id: request?.id ?? null, error: { code: -32000, message } })}\n`)
    }
  }
}

const invoked = process.argv[1] && samePath(fileURLToPath(import.meta.url), process.argv[1])
if (invoked) serve().catch(error => { process.stderr.write(`${String(error?.stack || error)}\n`); process.exitCode = 1 })

export { CANONICAL_REF, GATE_REGISTRY, MAIN_REF, OPERATION_REGISTRY, RECOVERY_SOURCE, TOOL_DEFINITIONS, ZERO_SHA }
