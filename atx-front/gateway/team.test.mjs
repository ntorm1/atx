import test from "node:test";
import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { createApp } from "./app.mjs";

const authorization = `Bearer ${"r".repeat(32)}`;
const nativeWith = (unary) => ({
  unary,
  watch: () => Object.assign(new EventEmitter(), { cancel() {} }),
});

test("team requests require a bounded browser credential before native work", async (t) => {
  let calls = 0;
  const app = await createApp({
    teamAuth: true,
    native: nativeWith(async () => {
      calls++;
      return { status: "SERVING" };
    }),
  });
  t.after(() => app.close());
  for (const value of [
    undefined,
    "Bearer short",
    `Bearer ${"a".repeat(257)}`,
    `Bearer ${"a".repeat(32)} `,
  ]) {
    const response = await app.inject({
      url: "/api/health",
      headers: value === undefined ? {} : { authorization: value },
    });
    assert.equal(response.statusCode, 401);
  }
  assert.equal(calls, 0);
});

test("team unary requests forward exactly their credential and expose current identity", async (t) => {
  const calls = [];
  const session = {
    team_mode: true,
    project_id: "local",
    team_name: "Research",
    principal: {
      id: "alice",
      name: "Alice",
      role: "researcher",
      expires_at_ms: "0",
    },
  };
  const app = await createApp({
    teamAuth: true,
    native: nativeWith(async (...args) => {
      calls.push(args);
      return session;
    }),
  });
  t.after(() => app.close());
  const response = await app.inject({
    url: "/api/session",
    headers: { authorization },
  });
  assert.equal(response.statusCode, 200);
  assert.deepEqual(response.json(), session);
  assert.equal(calls[0][0], "getSession");
  assert.equal(calls[0][3], authorization);
  assert.ok(calls[0][2] instanceof AbortSignal);
  await app.inject({ url: "/api/team", headers: { authorization } });
  assert.equal(calls[1][0], "listTeam");
  assert.equal(calls[1][3], authorization);
});

test("readiness uses only service health and returns no operational counters", async (t) => {
  const calls = [];
  const app = await createApp({
    teamAuth: true,
    native: nativeWith(async (...args) => {
      calls.push(args);
      return { status: "SERVING", cache_bytes: "65536", queued_jobs: "10" };
    }),
  });
  t.after(() => app.close());
  const response = await app.inject("/healthz");
  assert.equal(response.statusCode, 200);
  assert.deepEqual(response.json(), { status: "SERVING" });
  assert.equal(calls[0][0], "getHealth");
  assert.equal(calls[0][3], undefined);
});

test("shared browser passwords cannot accidentally bypass configured team authorization", async () => {
  await assert.rejects(
    createApp({
      teamAuth: true,
      webToken: "shared-password",
      native: nativeWith(async () => ({})),
    }),
    /cannot combine/i,
  );
});

test("shared event fanout authenticates the browser before admitting a stream", async (t) => {
  let calls = 0;
  const app = await createApp({
    teamAuth: true,
    native: nativeWith(async () => {
      calls++;
      throw Object.assign(new Error("Invalid credential"), { code: 16 });
    }),
  });
  t.after(() => app.close());
  const address = await app.listen({ host: "127.0.0.1", port: 0 });
  const response = await fetch(address + "/api/events", {
    headers: { authorization },
    signal: AbortSignal.timeout(2000),
  });
  await response.body.cancel();
  assert.equal(response.status, 401);
  assert.equal(calls, 1);
});

test("an expired native session cannot enter the shared event stream", async (t) => {
  const app = await createApp({
    teamAuth: true,
    native: nativeWith(async () => ({
      team_mode: true,
      principal: { id: "alice", role: "viewer", expires_at_ms: "1" },
    })),
  });
  t.after(() => app.close());
  const address = await app.listen({ host: "127.0.0.1", port: 0 });
  const response = await fetch(address + "/api/events", {
    headers: { authorization },
    signal: AbortSignal.timeout(2000),
  });
  await response.body.cancel();
  assert.equal(response.status, 401);
});

test("native restart closes team streams and revoked identities cannot reconnect", async (t) => {
  let revoked = false;
  let authentications = 0;
  const stream = Object.assign(new EventEmitter(), { cancel() {} });
  const app = await createApp({
    teamAuth: true,
    native: {
      watch: () => stream,
      unary: async (method, _body, _signal, credential) => {
        assert.equal(method, "getSession");
        assert.equal(credential, authorization);
        authentications++;
        if (revoked)
          throw Object.assign(new Error("Identity no longer configured"), {
            code: 16,
          });
        return {
          team_mode: true,
          principal: { id: "alice", role: "researcher", expires_at_ms: "0" },
        };
      },
    },
  });
  t.after(() => app.close());
  const address = await app.listen({ host: "127.0.0.1", port: 0 });
  const response = await fetch(address + "/api/events", {
    headers: { authorization },
    signal: AbortSignal.timeout(3000),
  });
  assert.equal(response.status, 200);
  const reader = response.body.getReader();
  const connected = await reader.read();
  assert.match(new TextDecoder().decode(connected.value), /connected/);
  revoked = true;
  stream.emit("end");
  let ended = false;
  for (let attempt = 0; attempt < 8; attempt++) {
    if ((await reader.read()).done) {
      ended = true;
      break;
    }
  }
  assert.ok(ended, "native interruption must end the existing browser stream");
  const reconnect = await fetch(address + "/api/events", {
    headers: { authorization },
    signal: AbortSignal.timeout(3000),
  });
  await reconnect.body.cancel();
  assert.equal(reconnect.status, 401);
  assert.equal(authentications, 2);
});
