import test from "node:test";
import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { createApp } from "./app.mjs";

const id = "a".repeat(32);
const alpha = "b".repeat(32);
const revision = "9007199254740993";
const recipe = { mode: "synthetic_demo", sessions: 32 };
const input = () => ({
  name: "Scheduled research",
  request: recipe,
  interval_ms: "60000",
  max_runs: 10,
  first_run_at_ms: String(Date.now() + 60_000),
  idempotency_key: "workflow-fixture",
});
const fixture = async (t, unary) => {
  const app = await createApp({
    native: {
      unary,
      watch: () => Object.assign(new EventEmitter(), { cancel() {} }),
    },
  });
  t.after(() => app.close());
  return app;
};

test("workflow creation freezes a recipe and forwards exact UTC interval and actor-free inputs", async (t) => {
  const calls = [];
  const app = await fixture(t, async (method, body) => {
    calls.push({ method, body });
    return {
      id,
      request_json: body.request_json,
      actor_id: "native-actor",
      revision: "1",
    };
  });
  const body = input();
  const response = await app.inject({
    method: "POST",
    url: "/api/workflows",
    payload: body,
  });
  assert.equal(response.statusCode, 201);
  assert.equal(calls[0].method, "createWorkflow");
  assert.equal(calls[0].body.interval_ms, "60000");
  assert.equal(calls[0].body.first_run_at_ms, body.first_run_at_ms);
  assert.equal(calls[0].body.actor_id, undefined);
  assert.deepEqual(JSON.parse(calls[0].body.request_json), recipe);
  assert.deepEqual(response.json().request, recipe);
  assert.equal(response.json().actor_id, "native-actor");
});

test("workflow sources require an exclusive immutable alpha revision or bounded recipe", async (t) => {
  const calls = [];
  const app = await fixture(t, async (method, body) => {
    calls.push({ method, body });
    return {
      id,
      request_json: JSON.stringify(recipe),
      alpha_id: alpha,
      alpha_revision: revision,
    };
  });
  const saved = { ...input(), alpha_id: alpha, alpha_revision: revision };
  delete saved.request;
  const accepted = await app.inject({
    method: "POST",
    url: "/api/workflows",
    payload: saved,
  });
  assert.equal(accepted.statusCode, 201);
  assert.equal(calls[0].body.alpha_revision, revision);
  assert.equal(calls[0].body.request_json, "");
  for (const payload of [
    { ...saved, request: recipe },
    { ...input(), alpha_id: alpha },
    { ...saved, alpha_revision: Number(revision) },
    { ...saved, alpha_revision: "9223372036854775808" },
    { ...input(), request: { token: "secret" } },
    { ...input(), request: { note: "é".repeat(32769) } },
  ]) {
    const response = await app.inject({
      method: "POST",
      url: "/api/workflows",
      payload,
    });
    assert.equal(response.statusCode, 400);
  }
  assert.equal(calls.length, 1);
});

test("workflow schedule, finite budgets, text and actor spoofing fail before native admission", async (t) => {
  let calls = 0;
  const app = await fixture(t, async () => {
    calls++;
    return {};
  });
  for (const change of [
    { interval_ms: 60000 },
    { interval_ms: "59999" },
    { interval_ms: "2592000001" },
    { interval_ms: "6e4" },
    { first_run_at_ms: 123 },
    { first_run_at_ms: "0" },
    { first_run_at_ms: "9223372036854775808" },
    { max_runs: 0 },
    { max_runs: 1001 },
    { max_runs: 1.5 },
    { max_runs: "10" },
    { actor_id: "admin" },
    { description: "é".repeat(257) },
    { idempotency_key: "é".repeat(65) },
    { name: " " },
    { name: "a".repeat(81) },
  ]) {
    const response = await app.inject({
      method: "POST",
      url: "/api/workflows",
      payload: { ...input(), ...change },
    });
    assert.equal(response.statusCode, 400, JSON.stringify(change));
  }
  assert.equal(calls, 0);
});

test("workflow pause preserves exact CAS conflicts and excludes configuration mutation", async (t) => {
  const calls = [];
  const app = await fixture(t, async (method, body) => {
    calls.push({ method, body });
    throw Object.assign(new Error("Lifecycle revision changed"), { code: 6 });
  });
  const valid = { expected_revision: revision, paused: true };
  const response = await app.inject({
    method: "POST",
    url: `/api/workflows/${id}/pause`,
    payload: valid,
  });
  assert.equal(response.statusCode, 409);
  assert.deepEqual(calls[0], {
    method: "setWorkflowPaused",
    body: { id, ...valid },
  });
  for (const payload of [
    { ...valid, paused: "false" },
    { ...valid, expected_revision: Number(revision) },
    { ...valid, request: recipe },
    { ...valid, actor_id: "admin" },
  ]) {
    assert.equal(
      (
        await app.inject({
          method: "POST",
          url: `/api/workflows/${id}/pause`,
          payload,
        })
      ).statusCode,
      400,
    );
  }
  assert.equal(calls.length, 1);
});

test("workflow summaries and paged run lineage omit heavy recipe and result documents", async (t) => {
  const calls = [];
  const row = {
    id,
    request_json: JSON.stringify(recipe),
    revision,
    admitted_runs: "2",
  };
  const app = await fixture(t, async (method, body) => {
    calls.push({ method, body });
    if (method === "listWorkflows") return { workflows: [row], total: "1" };
    if (method === "getWorkflow") return row;
    return {
      jobs: [
        {
          id: alpha,
          workflow_id: id,
          workflow_slot_ms: "1000",
          request_json: "{}",
          result_json: "{}",
        },
      ],
      total: "1",
    };
  });
  const list = await app.inject("/api/workflows?limit=100&offset=0");
  assert.equal(list.statusCode, 200);
  assert.equal(list.json().workflows[0].request, null);
  assert.equal(list.json().workflows[0].request_json, undefined);
  assert.equal(list.json().workflows[0].admitted_runs, "2");
  assert.deepEqual(
    (await app.inject(`/api/workflows/${id}`)).json().request,
    recipe,
  );
  const runs = await app.inject(`/api/workflows/${id}/runs?limit=50&offset=2`);
  assert.equal(runs.statusCode, 200);
  assert.deepEqual(calls[2], {
    method: "listWorkflowRuns",
    body: { id, limit: 50, offset: 2 },
  });
  assert.equal(runs.json().jobs[0].workflow_id, id);
  assert.equal(runs.json().jobs[0].request, null);
  assert.equal(runs.json().jobs[0].result, null);
  assert.equal((await app.inject("/api/workflows?limit=101")).statusCode, 400);
  assert.equal(
    (await app.inject(`/api/workflows/${id}/runs?offset=1000001`)).statusCode,
    400,
  );
});
