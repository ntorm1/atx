import test from "node:test";
import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { createApp } from "./app.mjs";

const id = "a".repeat(32);
const alpha = "b".repeat(32);
const child = "c".repeat(32);
const revision = "9007199254740993";
const recipe = { mode: "synthetic_demo", sessions: 32 };
const snapshot = {
  id,
  name: "Costs",
  description: "",
  created_at_ms: "1",
  scheduled_at_ms: "0",
  status: "queued",
  progress: 0,
  total: 2,
  queued: 2,
  running: 0,
  succeeded: 0,
  failed: 0,
  cancelled: 0,
  jobs: [
    {
      id: child,
      status: "queued",
      label: "Pinned alpha",
      alpha_id: alpha,
      alpha_revision: revision,
      experiment_id: id,
      request_json: "secret recipe",
      result_json: "secret result",
    },
  ],
};
const nativeWith = (unary) => ({
  unary,
  watch: () => Object.assign(new EventEmitter(), { cancel() {} }),
});
test("experiments preserve exact revisions and submit either raw recipes or immutable alpha links", async (t) => {
  const seen = [];
  const app = await createApp({
    native: nativeWith(async (method, body) => {
      seen.push({ method, body });
      return snapshot;
    }),
  });
  t.after(() => app.close());
  const response = await app.inject({
    method: "POST",
    url: "/api/experiments",
    payload: {
      name: "Costs",
      idempotency_key: "same-batch",
      simulations: [
        { label: "Raw", request: recipe },
        { label: "Pinned alpha", alpha_id: alpha, alpha_revision: revision },
      ],
    },
  });
  assert.equal(response.statusCode, 201);
  assert.deepEqual(seen, [
    {
      method: "createExperiment",
      body: {
        name: "Costs",
        description: "",
        idempotency_key: "same-batch",
        scheduled_at_ms: 0,
        simulations: [
          {
            label: "Raw",
            request_json: JSON.stringify(recipe),
            alpha_id: "",
            alpha_revision: "0",
          },
          {
            label: "Pinned alpha",
            request_json: "",
            alpha_id: alpha,
            alpha_revision: revision,
          },
        ],
      },
    },
  ]);
  const saved = response.json();
  assert.equal(saved.jobs[0].alpha_revision, revision);
  assert.equal(saved.jobs[0].request, null);
  assert.equal(saved.jobs[0].result, null);
  assert.equal("request_json" in saved.jobs[0], false);
  assert.equal("result_json" in saved.jobs[0], false);
});
test("experiment browse, detail and cancellation map native RPCs without reinterpreting terminal states", async (t) => {
  const seen = [];
  const app = await createApp({
    native: nativeWith(async (method, body) => {
      seen.push({ method, body });
      if (method === "listExperiments")
        return { experiments: [snapshot], total: "1" };
      return {
        ...snapshot,
        status: "succeeded",
        succeeded: 2,
        queued: 0,
        progress: 1,
      };
    }),
  });
  t.after(() => app.close());
  const page = await app.inject("/api/experiments?limit=25&offset=100");
  assert.equal(page.statusCode, 200);
  assert.deepEqual(seen[0], {
    method: "listExperiments",
    body: { limit: 25, offset: 100 },
  });
  assert.deepEqual(page.json().experiments[0].jobs, []);
  const detail = await app.inject(`/api/experiments/${id}`);
  assert.equal(detail.statusCode, 200);
  assert.equal(detail.json().jobs[0].alpha_revision, revision);
  const cancelled = await app.inject({
    method: "POST",
    url: `/api/experiments/${id}/cancel`,
  });
  assert.equal(cancelled.statusCode, 200);
  assert.equal(cancelled.json().status, "succeeded");
  assert.deepEqual(seen.slice(1), [
    { method: "getExperiment", body: { id } },
    { method: "cancelExperiment", body: { id } },
  ]);
});
test("experiment source exclusivity, revision precision, count and scheduling bounds reject before native work", async (t) => {
  let calls = 0;
  const app = await createApp({
    native: nativeWith(async () => {
      calls++;
      return snapshot;
    }),
  });
  t.after(() => app.close());
  for (const simulations of [
    null,
    {},
    [],
    Array.from({ length: 33 }, () => ({ request: recipe })),
    [{}],
    [{ alpha_id: alpha }],
    [{ alpha_revision: "1" }],
    [{ request: recipe, alpha_id: alpha, alpha_revision: "1" }],
    [{ alpha_id: alpha, alpha_revision: 1 }],
    [{ alpha_id: alpha, alpha_revision: revision, surprise: "ignored?" }],
    [{ alpha_id: alpha, alpha_revision: "0" }],
    [{ alpha_id: alpha, alpha_revision: "9223372036854775808" }],
    [{ alpha_id: alpha, alpha_revision: "01" }],
    [{ request: [] }],
    [{ request: null }],
  ]) {
    const response = await app.inject({
      method: "POST",
      url: "/api/experiments",
      payload: { name: "Costs", idempotency_key: "bounded", simulations },
    });
    assert.equal(response.statusCode, 400, JSON.stringify(simulations));
  }
  for (const scheduled_at_ms of [
    -1,
    1.5,
    Number.MAX_SAFE_INTEGER + 1,
    Date.now() + 366 * 86400000,
  ]) {
    assert.equal(
      (
        await app.inject({
          method: "POST",
          url: "/api/experiments",
          payload: {
            name: "Costs",
            idempotency_key: "bounded",
            simulations: [{ request: recipe }],
            scheduled_at_ms,
          },
        })
      ).statusCode,
      400,
    );
  }
  assert.equal(calls, 0);
});
test("experiment UTF-8, object depth and canonical aggregate byte budgets are enforced", async (t) => {
  let calls = 0;
  const app = await createApp({
    native: nativeWith(async () => {
      calls++;
      return snapshot;
    }),
  });
  t.after(() => app.close());
  let deep = {};
  for (let i = 0; i < 33; i++) deep = { child: deep };
  for (const overrides of [
    { name: "x".repeat(81) },
    { name: "bad\nname" },
    { name: "\ud800" },
    { description: "\u00e9".repeat(257) },
    { idempotency_key: "\u00e9".repeat(65) },
    { simulations: [{ label: "x".repeat(81), request: recipe }] },
    { simulations: [{ request: deep }] },
    { simulations: [{ request: { text: "x".repeat(65536) } }] },
    {
      simulations: [
        { request: { text: "x".repeat(50000) } },
        { request: { text: "x".repeat(50000) } },
      ],
    },
    { simulations: [{ request: { auth_token: "secret" } }] },
    { unknown: "value" },
  ])
    assert.equal(
      (
        await app.inject({
          method: "POST",
          url: "/api/experiments",
          payload: {
            name: "Costs",
            idempotency_key: "bounded",
            simulations: [{ request: recipe }],
            ...overrides,
          },
        })
      ).statusCode,
      400,
    );
  for (const path of [
    "/api/experiments?limit=101",
    "/api/experiments?offset=1000001",
    `/api/experiments/not-an-id`,
    "/api/experiments?unknown=true",
  ]) {
    assert.equal((await app.inject(path)).statusCode, 400);
  }
  assert.equal(calls, 0);
});
test("all experiment routes require gateway authorization and native errors retain shared safe mapping", async (t) => {
  let calls = 0,
    code = 8;
  const app = await createApp({
    webToken: "secret",
    native: nativeWith(async () => {
      calls++;
      throw Object.assign(new Error("native detail"), {
        code,
        details: "native detail",
      });
    }),
  });
  t.after(() => app.close());
  for (const route of [
    { url: "/api/experiments" },
    { url: `/api/experiments/${id}` },
    { method: "POST", url: "/api/experiments", payload: {} },
    { method: "POST", url: `/api/experiments/${id}/cancel` },
  ]) {
    assert.equal((await app.inject(route)).statusCode, 401);
  }
  assert.equal(calls, 0);
  for (const [nativeCode, expected] of [
    [3, 400],
    [5, 404],
    [6, 409],
    [8, 429],
    [14, 503],
  ]) {
    code = nativeCode;
    const response = await app.inject({
      url: `/api/experiments/${id}`,
      headers: { authorization: "Bearer secret" },
    });
    assert.equal(response.statusCode, expected);
    if (expected >= 500)
      assert.equal(response.json().error.includes("native detail"), false);
  }
});
test("UTF-8 boundaries, exact int64 limits and bounded cancellation bodies preserve native authority", async (t) => {
  let calls = 0;
  const app = await createApp({
    native: nativeWith(async () => {
      calls++;
      return snapshot;
    }),
  });
  t.after(() => app.close());
  const unicode = "\u{1f4c8}".repeat(80);
  const valid = await app.inject({
    method: "POST",
    url: "/api/experiments",
    payload: {
      name: unicode,
      description: "\u00e9".repeat(256),
      idempotency_key: "exact-max",
      simulations: [
        {
          alpha_id: alpha,
          alpha_revision: "9223372036854775807",
          label: unicode,
        },
      ],
    },
  });
  assert.equal(valid.statusCode, 201);
  assert.equal(calls, 1);
  for (const payload of [{ force: true }, [], null]) {
    assert.equal(
      (
        await app.inject({
          method: "POST",
          url: `/api/experiments/${id}/cancel`,
          headers: { "content-type": "application/json" },
          payload: JSON.stringify(payload),
        })
      ).statusCode,
      400,
    );
  }
  const large = await app.inject({
    method: "POST",
    url: "/api/experiments",
    payload: {
      name: "Large",
      idempotency_key: "body-limit",
      simulations: [{ request: { text: "x".repeat(300000) } }],
    },
  });
  assert.equal(large.statusCode, 413);
  assert.equal(calls, 1);
});
