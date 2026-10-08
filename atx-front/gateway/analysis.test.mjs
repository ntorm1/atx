import test from "node:test";
import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { createApp } from "./app.mjs";

const ids = ["a".repeat(32), "b".repeat(32)];
const nativeWith = (unary) => ({
  unary,
  watch: () => Object.assign(new EventEmitter(), { cancel() {} }),
});

test("analysis compares bounded run IDs and preserves native result semantics", async (t) => {
  let call;
  const document = {
    observations: 4,
    correlation: [
      [1, null],
      [null, null],
    ],
  };
  const app = await createApp({
    native: nativeWith(async (method, body) => {
      call = { method, body };
      return { comparison_json: JSON.stringify(document) };
    }),
  });
  t.after(() => app.close());
  const response = await app.inject({
    method: "POST",
    url: "/api/analysis/compare",
    payload: { job_ids: ids },
  });
  assert.equal(response.statusCode, 200);
  assert.deepEqual(response.json(), document);
  assert.equal(call.method, "compareJobs");
  assert.deepEqual(call.body.job_ids, ids);
  assert.equal(call.body.allow_mixed_evidence, false);
  assert.equal(call.body.max_chart_points, 1000);
});

test("analysis rejects unbounded, duplicate and malformed requests before native execution", async (t) => {
  let calls = 0;
  const app = await createApp({
    native: nativeWith(async () => {
      calls++;
      return {};
    }),
  });
  t.after(() => app.close());
  for (const payload of [
    { job_ids: [ids[0]] },
    { job_ids: [ids[0], ids[0]] },
    { job_ids: [...ids, "bad-id"] },
    { job_ids: ids, max_chart_points: 2001 },
    { job_ids: ids, unknown: true },
  ]) {
    const response = await app.inject({
      method: "POST",
      url: "/api/analysis/compare",
      payload,
    });
    assert.equal(response.statusCode, 400);
  }
  assert.equal(calls, 0);
});

test("expression validation forwards exact dataset revision and enforces UTF-8 byte limit", async (t) => {
  let call;
  const app = await createApp({
    native: nativeWith(async (method, body) => {
      call = { method, body };
      return { analysis_json: '{"valid":true,"required_lookback":5}' };
    }),
  });
  t.after(() => app.close());
  const payload = {
    strategy_dsl: "rank(delta(close,5))",
    dataset_id: "equities",
    dataset_sha256: "c".repeat(64),
  };
  const response = await app.inject({
    method: "POST",
    url: "/api/analysis/validate",
    payload,
  });
  assert.equal(response.statusCode, 200);
  assert.deepEqual(call, { method: "validateExpression", body: payload });
  assert.equal(response.json().required_lookback, 5);
  const invalid = await app.inject({
    method: "POST",
    url: "/api/analysis/validate",
    payload: { strategy_dsl: "α".repeat(600) },
  });
  assert.equal(invalid.statusCode, 400);
});

test("malformed analysis responses are hidden and authorization precedes native work", async (t) => {
  let calls = 0;
  const app = await createApp({
    webToken: "secret",
    native: nativeWith(async () => {
      calls++;
      return { comparison_json: "secret malformed output" };
    }),
  });
  t.after(() => app.close());
  assert.equal(
    (
      await app.inject({
        method: "POST",
        url: "/api/analysis/compare",
        payload: { job_ids: ids },
      })
    ).statusCode,
    401,
  );
  assert.equal(calls, 0);
  const response = await app.inject({
    method: "POST",
    url: "/api/analysis/compare",
    headers: { authorization: "Bearer secret" },
    payload: { job_ids: ids },
  });
  assert.equal(response.statusCode, 502);
  assert.doesNotMatch(response.body, /secret malformed/);
});
