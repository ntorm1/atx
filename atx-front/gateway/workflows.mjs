const failure = (message, code = 3) =>
  Object.assign(new Error(message), { code, details: message });
const object = (value) =>
  Boolean(value && typeof value === "object" && !Array.isArray(value));
const shape = (required, properties) => ({
  type: "object",
  additionalProperties: false,
  required,
  properties,
});
const id = { type: "string", pattern: "^[0-9a-f]{32}$" };
const positive = { type: "string", pattern: "^[1-9][0-9]{0,18}$" };
const maxInteger = 9223372036854775807n;

function text(value, bytes, field) {
  if (
    typeof value !== "string" ||
    !value.isWellFormed() ||
    value.includes("\0") ||
    Buffer.byteLength(value) > bytes
  )
    throw failure(`${field} exceeds its UTF-8 text limits`);
}
function integer(value, field, min = 1n, max = maxInteger) {
  if (
    typeof value !== "string" ||
    !/^[1-9][0-9]{0,18}$/.test(value) ||
    BigInt(value) < min ||
    BigInt(value) > max
  )
    throw failure(`${field} requires an exact bounded positive decimal string`);
  return value;
}
function recipe(value) {
  if (!object(value)) throw failure("Workflow recipe must be an object");
  const inspect = (child, depth) => {
    if (child && typeof child === "object") {
      if (depth > 32)
        throw failure("Workflow recipe exceeds JSON nesting limit 32");
      for (const [key, nested] of Object.entries(child)) {
        if (
          /^(?:__proto__|constructor|prototype|token|access_token|auth_token|authorization|password|credentials|connection_string)$/i.test(
            key,
          )
        )
          throw failure("Credentials cannot be saved in workflow recipes");
        text(key, 65536, "Recipe key");
        inspect(nested, depth + 1);
      }
    } else if (typeof child === "string") text(child, 65536, "Recipe text");
  };
  inspect(value, 1);
  const json = JSON.stringify(value);
  if (Buffer.byteLength(json) > 65536)
    throw failure("Workflow recipe exceeds 64 KiB");
  return json;
}
function write(body) {
  text(body.name, 320, "Workflow name");
  const trimmed = body.name.replace(
    /^\p{White_Space}+|\p{White_Space}+$/gu,
    "",
  );
  if (
    !trimmed ||
    [...trimmed].length > 80 ||
    /[\u0000-\u001f\u007f-\u009f]/u.test(trimmed)
  )
    throw failure("Workflow name requires 1..80 non-control characters");
  text(body.description, 512, "Workflow description");
  text(body.idempotency_key, 128, "Workflow idempotency key");
  const hasRecipe = Object.hasOwn(body, "request");
  const hasAlpha = Object.hasOwn(body, "alpha_id");
  const hasRevision = Object.hasOwn(body, "alpha_revision");
  if (hasAlpha !== hasRevision || hasRecipe === hasAlpha)
    throw failure(
      "Workflow requires one recipe or an exact alpha ID/revision pair",
    );
  return {
    name: body.name,
    description: body.description,
    request_json: hasRecipe ? recipe(body.request) : "",
    alpha_id: hasAlpha ? body.alpha_id : "",
    alpha_revision: hasAlpha
      ? integer(body.alpha_revision, "Alpha revision")
      : "0",
    interval_ms: integer(body.interval_ms, "UTC interval", 60000n, 2592000000n),
    first_run_at_ms: integer(
      body.first_run_at_ms,
      "First run",
      1n,
      BigInt(Date.now() + 365 * 86400000),
    ),
    max_runs: body.max_runs,
    idempotency_key: body.idempotency_key,
  };
}
function decode(record, summary = false) {
  const { request_json, ...metadata } = record;
  if (summary) return { ...metadata, request: null };
  if (
    typeof request_json !== "string" ||
    Buffer.byteLength(request_json) > 65536
  )
    throw failure("Invalid workflow recipe from native service", 13);
  try {
    const request = JSON.parse(request_json);
    if (!object(request)) throw new Error("object required");
    return { ...metadata, request };
  } catch {
    throw failure("Invalid workflow recipe from native service", 13);
  }
}

export function registerWorkflowRoutes(app, unary) {
  const params = shape(["id"], { id });
  const page = shape([], {
    limit: { type: "integer", minimum: 1, maximum: 100, default: 50 },
    offset: { type: "integer", minimum: 0, maximum: 1000000, default: 0 },
  });
  app.post(
    "/api/workflows",
    {
      bodyLimit: 128 * 1024,
      preValidation: async (req) => {
        for (const field of [
          "interval_ms",
          "first_run_at_ms",
          "alpha_revision",
        ])
          if (
            req.body &&
            Object.hasOwn(req.body, field) &&
            typeof req.body[field] !== "string"
          )
            throw failure(`${field} must be an exact decimal string`);
        if (
          req.body &&
          Object.hasOwn(req.body, "max_runs") &&
          typeof req.body.max_runs !== "number"
        )
          throw failure("max_runs must be a finite integer");
      },
      schema: {
        body: shape(
          [
            "name",
            "interval_ms",
            "max_runs",
            "first_run_at_ms",
            "idempotency_key",
          ],
          {
            name: { type: "string", minLength: 1, maxLength: 320 },
            description: { type: "string", maxLength: 512, default: "" },
            request: { type: "object" },
            alpha_id: id,
            alpha_revision: positive,
            interval_ms: positive,
            max_runs: { type: "integer", minimum: 1, maximum: 1000 },
            first_run_at_ms: positive,
            idempotency_key: { type: "string", minLength: 1, maxLength: 128 },
          },
        ),
      },
    },
    async (req, reply) => {
      const result = decode(
        await unary(req, "createWorkflow", write(req.body)),
      );
      reply.code(201);
      return result;
    },
  );
  app.get("/api/workflows", { schema: { querystring: page } }, async (req) => {
    const result = await unary(req, "listWorkflows", req.query);
    return {
      ...result,
      workflows: result.workflows.map((row) => decode(row, true)),
    };
  });
  app.get("/api/workflows/:id", { schema: { params } }, async (req) =>
    decode(await unary(req, "getWorkflow", req.params)),
  );
  app.post(
    "/api/workflows/:id/pause",
    {
      bodyLimit: 1024,
      preValidation: async (req) => {
        if (typeof req.body?.paused !== "boolean")
          throw failure("paused must be a boolean");
        integer(req.body?.expected_revision, "Expected revision");
      },
      schema: {
        params,
        body: shape(["expected_revision", "paused"], {
          expected_revision: positive,
          paused: { type: "boolean" },
        }),
      },
    },
    async (req) =>
      decode(
        await unary(req, "setWorkflowPaused", { ...req.params, ...req.body }),
      ),
  );
  app.get(
    "/api/workflows/:id/runs",
    { schema: { params, querystring: page } },
    async (req) => {
      const result = await unary(req, "listWorkflowRuns", {
        ...req.params,
        ...req.query,
      });
      return {
        ...result,
        jobs: result.jobs.map(
          ({ request_json: _request, result_json: _result, ...job }) => ({
            ...job,
            request: null,
            result: null,
          }),
        ),
      };
    },
  );
}
