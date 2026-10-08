import test from "node:test";
import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { createApp } from "./app.mjs";

const job = {
  id: "job-1",
  status: "queued",
  request_json: '{"mode":"synthetic_demo"}',
  result_json: "",
  error: "",
  progress: 0,
};
function fakeNative(overrides = {}) {
  return {
    unary: async (method, body) =>
      method === "listJobs"
        ? { jobs: [job], total: "1" }
        : method === "listStrategies"
          ? { strategies_json: '{"strategies":[]}' }
          : method === "getHealth"
            ? { status: "ok" }
            : job,
    watch() {
      return Object.assign(new EventEmitter(), {
        cancel() {},
        pause() {},
        resume() {},
      });
    },
    ...overrides,
  };
}

test("host allowlist blocks DNS rebinding before native calls", async (t) => {
  let calls = 0;
  const app = await createApp({
    native: fakeNative({
      unary: async () => {
        calls++;
        return {};
      },
    }),
  });
  t.after(() => app.close());
  assert.equal(
    (
      await app.inject({
        url: "/api/health",
        headers: { host: "attacker.example:3000" },
      })
    ).statusCode,
    403,
  );
  assert.equal(calls, 0);
  assert.equal(
    (
      await app.inject({
        url: "/api/health",
        headers: { host: "127.0.0.1:3000" },
      })
    ).statusCode,
    200,
  );
  assert.equal(calls, 1);
});
test("gateway requires separate browser authentication and does not call native when denied", async (t) => {
  let calls = 0;
  const app = await createApp({
    native: fakeNative({
      unary: async () => {
        calls++;
        return {};
      },
    }),
    webToken: "browser-secret",
  });
  t.after(() => app.close());
  const unauthorized = await app.inject("/api/health");
  assert.equal(unauthorized.statusCode, 401);
  assert.equal(calls, 0);
  assert.equal(
    (
      await app.inject({
        url: "/api/health",
        headers: { authorization: "Bearer browser-secret" },
      })
    ).statusCode,
    200,
  );
  assert.equal(calls, 1);
  assert.equal(
    (
      await app.inject({
        url: "/api/health",
        headers: { authorization: "Bearer browser-secret-extra" },
      })
    ).statusCode,
    401,
  );
});
test("request translation preserves idempotency and schedule and decodes native documents", async (t) => {
  let seen;
  const app = await createApp({
    native: fakeNative({
      unary: async (method, body) => {
        seen = { method, body };
        return job;
      },
    }),
  });
  t.after(() => app.close());
  const response = await app.inject({
    method: "POST",
    url: "/api/jobs",
    payload: {
      request: { mode: "import_nav", artifact_path: "results/nav.csv" },
      idempotency_key: "stable-key",
      scheduled_at_ms: 1234,
    },
  });
  assert.equal(response.statusCode, 201);
  assert.deepEqual(seen, {
    method: "submitJob",
    body: {
      request_json: '{"mode":"import_nav","artifact_path":"results/nav.csv"}',
      idempotency_key: "stable-key",
      scheduled_at_ms: 1234,
    },
  });
  assert.deepEqual(response.json().request, { mode: "synthetic_demo" });
  assert.equal(response.json().result, null);
  assert.equal(response.json().request_json, undefined);
});
test("cross-origin browser mutations are denied and same-origin requests accepted", async (t) => {
  const app = await createApp({ native: fakeNative() });
  t.after(() => app.close());
  const payload = {
    request: { mode: "synthetic_demo" },
    idempotency_key: "key",
  };
  assert.equal(
    (
      await app.inject({
        method: "POST",
        url: "/api/jobs",
        payload,
        headers: { origin: "https://attacker.example", host: "127.0.0.1:3000" },
      })
    ).statusCode,
    403,
  );
  assert.equal(
    (
      await app.inject({
        method: "POST",
        url: "/api/jobs",
        payload,
        headers: { origin: "null", host: "127.0.0.1:3000" },
      })
    ).statusCode,
    403,
  );
  assert.equal(
    (
      await app.inject({
        method: "POST",
        url: "/api/jobs",
        payload,
        headers: { "sec-fetch-site": "cross-site" },
      })
    ).statusCode,
    403,
  );
  assert.equal(
    (
      await app.inject({
        method: "POST",
        url: "/api/jobs",
        payload,
        headers: { origin: "http://127.0.0.1:3000", host: "127.0.0.1:3000" },
      })
    ).statusCode,
    201,
  );
});
test("pagination, query and body bounds reject invalid inputs", async (t) => {
  const app = await createApp({ native: fakeNative() });
  t.after(() => app.close());
  for (const url of [
    "/api/jobs?limit=201",
    "/api/jobs?limit=0",
    "/api/jobs?offset=-1",
    "/api/jobs?status=made-up",
    "/api/events?channel=unknown",
    "/api/events?after_sequence=bad",
    "/api/events?after_sequence=99999999999999999999",
  ])
    assert.equal((await app.inject(url)).statusCode, 400, url);
  assert.equal(
    (
      await app.inject({
        method: "POST",
        url: "/api/jobs",
        payload: { request: {} },
      })
    ).statusCode,
    400,
  );
  const list = await app.inject("/api/jobs?limit=20");
  assert.equal(list.statusCode, 200);
  assert.equal(list.json().jobs[0].id, "job-1");
});
test("native statuses map to accurate browser status and internal errors stay private", async (t) => {
  let code = 5;
  const app = await createApp({
    native: fakeNative({
      unary: async () => {
        throw Object.assign(new Error("private implementation path"), {
          code,
          details: "Job not found",
        });
      },
    }),
  });
  t.after(() => app.close());
  assert.equal((await app.inject("/api/jobs/missing")).statusCode, 404);
  code = 3;
  assert.equal((await app.inject("/api/health")).statusCode, 400);
  code = 8;
  assert.equal((await app.inject("/api/health")).statusCode, 429);
  code = 14;
  const unavailable = await app.inject("/api/health");
  assert.equal(unavailable.statusCode, 503);
  assert.ok(!unavailable.body.includes("private implementation path"));
});
test("malformed native result documents never become success responses", async (t) => {
  const app = await createApp({
    native: fakeNative({
      unary: async () => ({ ...job, result_json: "{broken" }),
    }),
  });
  t.after(() => app.close());
  assert.equal((await app.inject("/api/jobs/job-1")).statusCode, 502);
});
test("shutdown drains open event responses and cancels native streams", async (t) => {
  let cancelled = false;
  const stream = Object.assign(new EventEmitter(), {
    cancel() {
      cancelled = true;
    },
    pause() {},
    resume() {},
  });
  const app = await createApp({ native: fakeNative({ watch: () => stream }) });
  await app.listen({ host: "127.0.0.1", port: 0 });
  t.after(() => app.close());
  const response = await fetch(
    `http://127.0.0.1:${app.server.address().port}/api/events`,
  );
  const reader = response.body.getReader();
  await reader.read();
  const stopped = app.close();
  assert.equal((await reader.read()).done, true);
  await stopped;
  assert.equal(cancelled, true);
});
test("oversized native events end the stream before buffering an unbounded response", async (t) => {
  let cancelled = false;
  const stream = Object.assign(new EventEmitter(), {
    cancel() {
      cancelled = true;
    },
    pause() {},
    resume() {},
  });
  const app = await createApp({ native: fakeNative({ watch: () => stream }) });
  await app.listen({ host: "127.0.0.1", port: 0 });
  t.after(() => app.close());
  const response = await fetch(
    `http://127.0.0.1:${app.server.address().port}/api/events`,
  );
  const reader = response.body.getReader();
  await reader.read();
  stream.emit("data", {
    sequence: "1",
    channel: "jobs",
    type: "job.progress",
    job_id: "job-1",
    gap: false,
    payload_json: JSON.stringify({ huge: "x".repeat(300 * 1024) }),
  });
  assert.match(
    new TextDecoder().decode((await reader.read()).value),
    /snapshot_required/,
  );
  assert.equal((await reader.read()).done, true);
  assert.equal(cancelled, true);
});
test("SSE relays sequence and gap, bounds connections, preserves upstream on browser disconnect", async (t) => {
  const calls = [];
  const native = fakeNative({
    watch(body) {
      const stream = Object.assign(new EventEmitter(), {
        cancelled: false,
        cancel() {
          this.cancelled = true;
        },
        pause() {},
        resume() {},
      });
      calls.push({ body, stream });
      return stream;
    },
  });
  const app = await createApp({ native, maxStreams: 1 });
  await app.listen({ host: "127.0.0.1", port: 0 });
  t.after(() => app.close());
  const base = `http://127.0.0.1:${app.server.address().port}`;
  const controller = new AbortController();
  const response = await fetch(
    `${base}/api/events?channel=jobs&after_sequence=9&job_id=job-1`,
    { signal: controller.signal },
  );
  assert.equal(response.status, 200);
  assert.deepEqual(calls[0].body, {
    channel: "all",
    after_sequence: "0",
    job_id: "",
  });
  assert.equal((await fetch(`${base}/api/events`)).status, 429);
  const reader = response.body.getReader();
  await reader.read();
  calls[0].stream.emit("data", {
    sequence: "10",
    channel: "jobs",
    type: "gap",
    job_id: "job-1",
    payload_json: '{"reason":"overflow"}',
    gap: true,
  });
  const next = new TextDecoder().decode((await reader.read()).value);
  assert.match(next, /id: 10/);
  assert.match(next, /"gap":true/);
  assert.match(next, /"payload":\{"reason":"overflow"\}/);
  controller.abort();
  await reader.cancel().catch(() => {});
  await new Promise((resolve) => setTimeout(resolve, 25));
  assert.equal(calls[0].stream.cancelled, false);
  await app.close();
  assert.equal(calls[0].stream.cancelled, true);
});
