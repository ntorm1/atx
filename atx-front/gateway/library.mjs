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
const id = { type: "string", pattern: "^[a-zA-Z0-9_-]{1,128}$" };
const revision = { type: "string", pattern: "^[1-9][0-9]{0,18}$" };
const paging = {
  limit: { type: "integer", minimum: 1, maximum: 100, default: 100 },
  offset: { type: "integer", minimum: 0, maximum: 1000000, default: 0 },
};
function recipe(value) {
  if (!object(value)) throw failure("Alpha recipe must be an object");
  const text = JSON.stringify(value);
  if (Buffer.byteLength(text) > 65536)
    throw failure("Alpha recipe exceeds 64 KiB");
  const inspect = (child, depth) => {
    if (depth > 32) throw failure("Alpha recipe exceeds the depth limit");
    if (child && typeof child === "object")
      for (const [key, nested] of Object.entries(child)) {
        if (
          /^(?:__proto__|constructor|prototype|token|access_token|auth_token|authorization|password|credentials|connection_string)$/i.test(
            key,
          )
        )
          throw failure("Credentials cannot be saved in alpha recipes");
        inspect(nested, depth + 1);
      }
  };
  inspect(value, 0);
  return text;
}
export const decodeAlpha = (record) => {
  const { request_json, ...metadata } = record;
  try {
    const request = JSON.parse(request_json);
    recipe(request);
    return { ...metadata, request };
  } catch {
    throw failure("Invalid alpha recipe from native service", 13);
  }
};
export function registerLibraryRoutes(app, unary) {
  const params = envelope(["id"], { id });
  const metadata = {
    name: { type: "string", minLength: 1, maxLength: 160 },
    description: { type: "string", maxLength: 512, default: "" },
    tags: {
      type: "array",
      maxItems: 16,
      uniqueItems: true,
      items: { type: "string", minLength: 1, maxLength: 64 },
      default: [],
    },
    hypothesis: { type: "string", maxLength: 4096, default: "" },
    request: { type: "object" },
    source_job_id: { ...id, pattern: "^[a-zA-Z0-9_-]{0,128}$", default: "" },
  };
  const exactRevision = async (req) => {
    if (
      typeof req.body?.expected_revision !== "string" ||
      !/^[1-9][0-9]{0,18}$/.test(req.body.expected_revision) ||
      BigInt(req.body.expected_revision) > 9223372036854775807n
    )
      throw failure(
        "Expected revision must be an exact positive int64 decimal string",
      );
  };
  const write = (body) => {
    if (
      !body.name.trim() ||
      [...body.name].length > 80 ||
      Buffer.byteLength(body.name) > 320
    )
      throw failure("Alpha name must contain 1 to 80 characters");
    for (const [field, max] of [
      ["description", 512],
      ["hypothesis", 4096],
    ])
      if (Buffer.byteLength(body[field] ?? "") > max)
        throw failure(`Alpha ${field} exceeds its UTF-8 byte limit`);
    for (const tag of body.tags ?? [])
      if (tag.trim() !== tag || Buffer.byteLength(tag) > 64)
        throw failure(
          "Alpha tags must be trimmed and contain at most 64 UTF-8 bytes",
        );
    const { request, ...rest } = body;
    return { ...rest, request_json: recipe(request) };
  };
  const summaries = (page) => ({
    ...page,
    alphas: page.alphas?.map(
      ({ request_json: _request, hypothesis: _hypothesis, ...record }) =>
        record,
    ),
    revisions: page.revisions?.map(
      ({ request_json: _request, hypothesis: _hypothesis, ...record }) =>
        record,
    ),
  });
  app.get(
    "/api/alphas",
    {
      schema: {
        querystring: envelope([], {
          ...paging,
          search: { type: "string", maxLength: 128, default: "" },
          tag: { type: "string", maxLength: 64, default: "" },
          project_id: { type: "string", enum: ["", "local"], default: "local" },
          include_archived: { type: "boolean", default: false },
        }),
      },
    },
    async (req) => {
      for (const [key, max] of [
        ["search", 128],
        ["tag", 64],
      ])
        if (Buffer.byteLength(req.query[key]) > max)
          throw failure(`Library ${key} exceeds its UTF-8 byte limit`);
      return summaries(await unary(req, "listAlphas", req.query));
    },
  );
  app.get(
    "/api/alphas/:id",
    {
      schema: {
        params,
        querystring: envelope([], {
          revision: {
            type: "string",
            pattern: "^(?:0|[1-9][0-9]{0,18})$",
            default: "0",
          },
        }),
      },
    },
    async (req) => {
      if (BigInt(req.query.revision) > 9223372036854775807n)
        throw failure("Revision exceeds int64 range");
      return decodeAlpha(
        await unary(req, "getAlpha", { ...req.params, ...req.query }),
      );
    },
  );
  app.post(
    "/api/alphas",
    {
      bodyLimit: 128 * 1024,
      schema: {
        body: envelope(["name", "request"], {
          ...metadata,
          project_id: { type: "string", enum: ["", "local"], default: "local" },
        }),
      },
    },
    async (req, reply) => {
      const created = decodeAlpha(
        await unary(req, "createAlpha", write(req.body)),
      );
      reply.code(201);
      return created;
    },
  );
  app.put(
    "/api/alphas/:id",
    {
      bodyLimit: 128 * 1024,
      preValidation: exactRevision,
      schema: {
        params,
        body: envelope(["name", "request", "expected_revision"], {
          ...metadata,
          expected_revision: revision,
        }),
      },
    },
    async (req) =>
      decodeAlpha(
        await unary(req, "updateAlpha", { ...req.params, ...write(req.body) }),
      ),
  );
  app.get(
    "/api/alphas/:id/revisions",
    { schema: { params, querystring: envelope([], { ...paging }) } },
    async (req) =>
      summaries(
        await unary(req, "listAlphaRevisions", { ...req.params, ...req.query }),
      ),
  );
  app.post(
    "/api/alphas/:id/archive",
    {
      preValidation: exactRevision,
      schema: {
        params,
        body: envelope(["expected_revision", "archived"], {
          expected_revision: revision,
          archived: { type: "boolean" },
        }),
      },
    },
    async (req) =>
      decodeAlpha(
        await unary(req, "setAlphaArchived", { ...req.params, ...req.body }),
      ),
  );
}
