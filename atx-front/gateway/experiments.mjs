const failure = (message, code = 3) =>
  Object.assign(new Error(message), { code, details: message });
const object = (value) =>
  Boolean(value && typeof value === "object" && !Array.isArray(value));
const envelope = (required, properties) => ({
  type: "object",
  additionalProperties: false,
  required,
  properties,
});
const id = { type: "string", pattern: "^[0-9a-f]{32}$" };
const revision = { type: "string", pattern: "^[1-9][0-9]{0,18}$" };
const maxRevision = 9223372036854775807n;

function text(value, maxBytes, field) {
  if (
    typeof value !== "string" ||
    !value.isWellFormed() ||
    value.includes("\0") ||
    Buffer.byteLength(value, "utf8") > maxBytes
  )
    throw failure(`${field} exceeds its UTF-8 text limits`);
  return value;
}
function name(value, field) {
  text(value, 320, field);
  const trimmed = value.replace(/^\p{White_Space}+|\p{White_Space}+$/gu, "");
  if (
    !trimmed ||
    [...trimmed].length > 80 ||
    /[\u0000-\u001f\u007f-\u009f]/u.test(trimmed)
  )
    throw failure(`${field} must contain 1 to 80 non-control characters`);
  return value;
}
function recipe(value) {
  if (!object(value)) throw failure("Simulation recipe must be an object");
  // Check container depth before stringify, which otherwise can overflow on deeply nested input.
  const inspect = (child, depth) => {
    if (child && typeof child === "object") {
      if (depth > 32)
        throw failure("Simulation recipe exceeds JSON nesting limit 32");
      for (const [key, nested] of Object.entries(child)) {
        if (
          /^(?:__proto__|constructor|prototype|token|access_token|auth_token|authorization|password|credentials|connection_string)$/i.test(
            key,
          )
        )
          throw failure("Credentials cannot be saved in simulation recipes");
        text(key, 65536, "Recipe key");
        inspect(nested, depth + 1);
      }
    } else if (typeof child === "string") text(child, 65536, "Recipe text");
  };
  inspect(value, 1);
  const json = JSON.stringify(value);
  if (Buffer.byteLength(json, "utf8") > 65536)
    throw failure("Simulation recipe exceeds 64 KiB");
  return json;
}
function write(body) {
  name(body.name, "Experiment name");
  text(body.description, 512, "Experiment description");
  text(body.idempotency_key, 128, "Experiment idempotency key");
  const schedule = body.scheduled_at_ms;
  if (
    !Number.isSafeInteger(schedule) ||
    schedule < 0 ||
    schedule > Date.now() + 365 * 86400000
  )
    throw failure(
      "Experiment schedule must be a safe millisecond integer within one year",
    );
  let requestBytes = 0;
  const simulations = body.simulations.map((simulation) => {
    const hasRequest = Object.hasOwn(simulation, "request");
    const hasAlpha = Object.hasOwn(simulation, "alpha_id");
    const hasRevision = Object.hasOwn(simulation, "alpha_revision");
    if (hasAlpha !== hasRevision || hasRequest === hasAlpha)
      throw failure(
        "Each simulation requires either a recipe or an exact alpha ID/revision pair",
      );
    if (hasRevision && BigInt(simulation.alpha_revision) > maxRevision)
      throw failure("Alpha revision exceeds the native positive int64 range");
    if (simulation.label) name(simulation.label, "Simulation label");
    const request_json = hasRequest ? recipe(simulation.request) : "";
    requestBytes += Buffer.byteLength(request_json, "utf8");
    if (requestBytes > 96 * 1024)
      throw failure("Experiment canonical recipes exceed 96 KiB");
    return {
      label: simulation.label,
      request_json,
      alpha_id: hasAlpha ? simulation.alpha_id : "",
      alpha_revision: hasAlpha ? simulation.alpha_revision : "0",
    };
  });
  return {
    name: body.name,
    description: body.description,
    idempotency_key: body.idempotency_key,
    scheduled_at_ms: schedule,
    simulations,
  };
}
function decode(record, summary = false) {
  const { jobs, ...metadata } = record;
  return {
    ...metadata,
    jobs: summary
      ? []
      : (jobs ?? []).map(
          ({ request_json: _request, result_json: _result, ...job }) => ({
            ...job,
            request: null,
            result: null,
          }),
        ),
  };
}

export function registerExperimentRoutes(app, unary) {
  const params = envelope(["id"], { id });
  const preValidation = async (req) => {
    if (
      req.body?.simulations !== undefined &&
      !Array.isArray(req.body.simulations)
    )
      throw failure("Experiment simulations must be an array");
    for (const simulation of req.body?.simulations ?? []) {
      if (
        simulation &&
        Object.hasOwn(simulation, "alpha_revision") &&
        typeof simulation.alpha_revision !== "string"
      )
        throw failure("Alpha revision must be an exact decimal string");
    }
  };
  app.post(
    "/api/experiments",
    {
      bodyLimit: 256 * 1024,
      preValidation,
      schema: {
        body: envelope(["name", "simulations", "idempotency_key"], {
          name: { type: "string", minLength: 1, maxLength: 320 },
          description: { type: "string", maxLength: 512, default: "" },
          idempotency_key: { type: "string", minLength: 1, maxLength: 128 },
          scheduled_at_ms: {
            type: "integer",
            minimum: 0,
            maximum: Number.MAX_SAFE_INTEGER,
            default: 0,
          },
          simulations: {
            type: "array",
            minItems: 1,
            maxItems: 32,
            items: envelope([], {
              label: { type: "string", maxLength: 320, default: "" },
              request: { type: "object" },
              alpha_id: id,
              alpha_revision: revision,
            }),
          },
        }),
      },
    },
    async (req, reply) => {
      const result = decode(
        await unary(req, "createExperiment", write(req.body)),
      );
      reply.code(201);
      return result;
    },
  );
  app.get(
    "/api/experiments",
    {
      schema: {
        querystring: envelope([], {
          limit: { type: "integer", minimum: 1, maximum: 100, default: 50 },
          offset: { type: "integer", minimum: 0, maximum: 1000000, default: 0 },
        }),
      },
    },
    async (req) => {
      const result = await unary(req, "listExperiments", { ...req.query });
      return {
        ...result,
        experiments: (result.experiments ?? []).map((record) =>
          decode(record, true),
        ),
      };
    },
  );
  app.get("/api/experiments/:id", { schema: { params } }, async (req) =>
    decode(await unary(req, "getExperiment", { ...req.params })),
  );
  app.post(
    "/api/experiments/:id/cancel",
    {
      schema: { params },
      preValidation: async (req) => {
        if (
          req.body !== undefined &&
          (!object(req.body) || Object.keys(req.body).length)
        )
          throw failure("Experiment cancellation accepts no options");
      },
    },
    async (req) =>
      decode(await unary(req, "cancelExperiment", { ...req.params })),
  );
}
