import assert from 'node:assert/strict'
import fs from 'node:fs'
import vm from 'node:vm'

const workflowPaths = [
  '.claude/workflows/vol-oracle-iter.js',
  '.claude/workflows/vol-sprint.js',
]
const sources = new Map(workflowPaths.map(path => [path, fs.readFileSync(path, 'utf8')]))
const forbiddenRuntimeApi = /(?:\bDate\s*\.\s*now\s*\(|\bnew\s+Date\s*\(|\bMath\s*\.\s*random\s*\(|\brandomUUID\s*\()/

for (const [path, source] of sources) {
  assert.equal(forbiddenRuntimeApi.test(source), false, `${path} uses a nondeterministic runtime API`)
  assert.equal(source.includes('\r'), false, `${path} must retain LF line endings`)
  assert.equal(/[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]/.test(source), false, `${path} contains a forbidden control byte`)
}

function loadFunction(path, name) {
  const source = sources.get(path)
  const start = source.indexOf(`function ${name}(`)
  assert.notEqual(start, -1, `${name} is missing from ${path}`)
  const open = source.indexOf('{', start)
  let depth = 0
  for (let index = open; index < source.length; index += 1) {
    if (source[index] === '{') depth += 1
    if (source[index] === '}') depth -= 1
    if (depth === 0) return vm.runInNewContext(`(${source.slice(start, index + 1)})`)
  }
  assert.fail(`${name} is unterminated in ${path}`)
}

const sprintPath = '.claude/workflows/vol-sprint.js'
const oraclePath = '.claude/workflows/vol-oracle-iter.js'
const deterministicToken = loadFunction(sprintPath, 'deterministicToken')
const sprintRunId = loadFunction(sprintPath, 'sprintRunId')
const laneHeartbeatId = loadFunction(sprintPath, 'laneHeartbeatId')
const oracleRunId = loadFunction(oraclePath, 'oracleRunId')
const shaA = 'a'.repeat(40)
const shaB = 'b'.repeat(40)

function compileWorkflow(path) {
  const body = sources.get(path).replace('export const meta', 'const meta')
  return new Function('args', 'agent', 'phase', 'workflow', 'pipeline', `return (async () => {\n${body}\n})()`)
}

async function startSprint(task) {
  const prompts = []
  const agent = async prompt => {
    prompts.push(prompt)
    if (prompts.length === 1) return {
      base_ref: 'main', base_sha: shaA,
      evidence: [{ command: 'git rev-parse main^{commit}', exit_code: 0, output: shaA }],
    }
    return { lanes: [], integration_branch: '', shared_files_note: '' }
  }
  await assert.rejects(
    compileWorkflow(sprintPath)({ task, base: 'main' }, agent, () => {}, async () => {}, async () => {}),
    /planner must return 1-4 mandatory lanes/,
  )
  assert.equal(prompts.length, 2, 'sprint startup did not reach the mocked planner')
  return prompts[1].match(/Run id: ([A-Za-z0-9._-]+)/)?.[1]
}

async function startOracle(state, nextIter = 'iter-0') {
  const agent = async (prompt, options) => {
    if (options?.label === 'capability') return {
      state, canonical_ref: 'refs/heads/oracle/canonical', canonical_exists: false,
      base_ref: 'main', base_sha: shaA,
      holdout_digest_receipt: state === 'missing_data' ? '' : 'd'.repeat(64), next_iter: nextIter,
      evidence: [{
        command: 'powershell scripts\\oracle-capability.ps1', exit_code: 0,
        output: `state=${state}\ncanonical_exists=false`,
      }],
    }
    return null
  }
  const result = await compileWorkflow(oraclePath)({}, agent, () => {}, async () => {}, async () => {})
  assert.equal(result.verdict, 'FAILED', 'oracle startup mock did not stop at the bounded bootstrap report')
  return result.run_id
}

const sprintContext = vm.createContext({ deterministicToken })
const sprintId = vm.runInContext(`(${sprintRunId.toString()})('${shaA}', 'task-a', '')`, sprintContext)
assert.equal(sprintId, vm.runInContext(`(${sprintRunId.toString()})('${shaA}', 'task-a', '')`, sprintContext), 'sprint ID changed for identical workflow state')
assert.notEqual(sprintId, vm.runInContext(`(${sprintRunId.toString()})('${shaB}', 'task-a', '')`, sprintContext), 'sprint ID ignored frozen base SHA')
assert.notEqual(sprintId, vm.runInContext(`(${sprintRunId.toString()})('${shaA}', 'task-b', '')`, sprintContext), 'sprint ID ignored task identity')
assert.notEqual(
  vm.runInContext(`(${sprintRunId.toString()})('${shaA}', 'same-task', 'caller-a')`, sprintContext),
  vm.runInContext(`(${sprintRunId.toString()})('${shaA}', 'same-task', 'caller-b')`, sprintContext),
  'distinct deterministic callers collided',
)
assert.notEqual(
  vm.runInContext(`(${sprintRunId.toString()})('${shaA}', 'task-a', 'same-caller')`, sprintContext),
  vm.runInContext(`(${sprintRunId.toString()})('${shaA}', 'task-b', 'same-caller')`, sprintContext),
  'distinct tasks from the same caller collided',
)
assert.notEqual(
  vm.runInContext(`(${sprintRunId.toString()})('${shaA}', 'task-a', '')`, sprintContext),
  vm.runInContext(`(${sprintRunId.toString()})('${shaA}', 'task-a', 'direct')`, sprintContext),
  'direct-call fallback collided with an explicit caller',
)

const laneContext = vm.createContext({ RUN_SLUG: sprintId })
const laneA = vm.runInContext(`(${laneHeartbeatId.toString()})({ id: 'lane-a' })`, laneContext)
const laneB = vm.runInContext(`(${laneHeartbeatId.toString()})({ id: 'lane-b' })`, laneContext)
assert.equal(laneA, vm.runInContext(`(${laneHeartbeatId.toString()})({ id: 'lane-a' })`, laneContext), 'lane owner ID changed across resume')
assert.notEqual(laneA, laneB, 'distinct concurrent lane identities collided')

const oracleBootstrap = oracleRunId(shaA, 'missing_data', '')
assert.equal(oracleBootstrap, oracleRunId(shaA, 'missing_data', ''), 'oracle bootstrap ID changed for identical capability state')
assert.notEqual(oracleBootstrap, oracleRunId(shaA, 'missing_mode_a', ''), 'oracle bootstrap stages collided')
assert.notEqual(oracleRunId(shaA, 'ready', 'iter-1'), oracleRunId(shaA, 'ready', 'iter-2'), 'oracle iterations collided')
assert.notEqual(oracleBootstrap, oracleRunId(shaB, 'missing_data', ''), 'oracle ID ignored frozen base SHA')

const startedSprint = await startSprint('task-a')
assert.equal(startedSprint, await startSprint('task-a'), 'actual sprint startup changed ID across replay')
assert.notEqual(startedSprint, await startSprint('task-b'), 'actual sprint startup collided across tasks')
const startedOracle = await startOracle('missing_data')
assert.equal(startedOracle, await startOracle('missing_data'), 'actual oracle startup changed ID across replay')
assert.notEqual(startedOracle, await startOracle('missing_mode_a'), 'actual oracle startup collided across stages')

console.log('workflow determinism contract: PASS')
