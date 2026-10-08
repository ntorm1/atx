const failure = (message, code = 3) =>
  Object.assign(new Error(message), { code, details: message });
const object = (value) =>
  Boolean(value && typeof value === "object" && !Array.isArray(value));
export function workspaceDocument(value) {
  if (
    !object(value) ||
    value.schema_version !== 1 ||
    !object(value.layout) ||
    !object(value.panel_state)
  )
    throw failure(
      "Workspace requires a version 1 layout and panel state document",
    );
  const document = JSON.stringify(value);
  if (Buffer.byteLength(document, "utf8") > 96 * 1024)
    throw failure("Workspace document exceeds 96 KiB");
  const inspect = (item, depth) => {
    if (depth > 32) throw failure("Workspace document exceeds the depth limit");
    if (item && typeof item === "object")
      for (const [key, child] of Object.entries(item)) {
        if (
          /^(?:__proto__|constructor|prototype|token|access_token|auth_token|authorization|password|credentials|connection_string)$/i.test(
            key,
          )
        )
          throw failure(
            "Credentials and unsafe object keys cannot be saved in workspaces",
          );
        inspect(child, depth + 1);
      }
  };
  inspect(value, 0);
  return document;
}
export const decodeWorkspace = (record) => {
  const { document_json, ...metadata } = record;
  let document;
  try {
    document = JSON.parse(document_json);
    workspaceDocument(document);
  } catch {
    throw failure("Invalid workspace document from native service", 13);
  }
  return { ...metadata, document };
};
export function registerWorkspaceRoutes(app, unary) {
  const id = {
    type: "string",
    minLength: 1,
    maxLength: 128,
    pattern: "^[a-zA-Z0-9_-]+$",
  };
  const revision = { type: "string", pattern: "^[1-9][0-9]{0,19}$" };
  const metadata = {
    name: { type: "string", minLength: 1, maxLength: 160 },
    description: { type: "string", maxLength: 512, default: "" },
    document: {
      type: "object",
      required: ["schema_version", "layout", "panel_state"],
      additionalProperties: false,
      properties: {
        schema_version: { const: 1 },
        layout: { type: "object" },
        panel_state: { type: "object" },
      },
    },
  };
  const envelope = (required, properties) => ({
    type: "object",
    additionalProperties: false,
    required,
    properties,
  });
  const params = envelope(["id"], { id });
  const requireRevisionString = async (req) => {
    if (typeof req.body?.expected_revision !== "string")
      throw failure(
        "Workspace expected_revision must be an exact decimal string",
      );
  };
  const checkRevision = (value) => {
    if (BigInt(value) > 18446744073709551615n)
      throw failure("Workspace revision exceeds uint64 range");
  };
  const writeBody = (body) => {
    if (
      !body.name.trim() ||
      [...body.name].length > 80 ||
      Buffer.byteLength(body.name) > 320
    )
      throw failure(
        "Workspace name must contain 1 to 80 characters (maximum 320 UTF-8 bytes)",
      );
    if (Buffer.byteLength(body.description ?? "") > 512)
      throw failure("Workspace description exceeds 512 UTF-8 bytes");
    if (body.expected_revision) checkRevision(body.expected_revision);
    const { document, ...rest } = body;
    return { ...rest, document_json: workspaceDocument(document) };
  };
  app.get(
    "/api/workspaces",
    {
      schema: {
        querystring: envelope([], {
          limit: { type: "integer", minimum: 1, maximum: 100, default: 100 },
          offset: {
            type: "integer",
            minimum: 0,
            maximum: 1_000_000,
            default: 0,
          },
        }),
      },
    },
    async (req) => {
      const result = await unary(req, "listWorkspaces", { ...req.query });
      return {
        total: result.total,
        workspaces: result.workspaces.map(
          ({ document_json: _document, ...record }) => record,
        ),
      };
    },
  );
  app.get("/api/workspaces/:id", { schema: { params } }, async (req) =>
    decodeWorkspace(await unary(req, "getWorkspace", req.params)),
  );
  app.post(
    "/api/workspaces",
    {
      bodyLimit: 128 * 1024,
      schema: { body: envelope(["name", "document"], metadata) },
    },
    async (req, reply) => {
      const result = decodeWorkspace(
        await unary(req, "createWorkspace", writeBody(req.body)),
      );
      reply.code(201);
      return result;
    },
  );
  app.put(
    "/api/workspaces/:id",
    {
      bodyLimit: 128 * 1024,
      preValidation: requireRevisionString,
      schema: {
        params,
        body: envelope(["name", "document", "expected_revision"], {
          ...metadata,
          expected_revision: revision,
        }),
      },
    },
    async (req) =>
      decodeWorkspace(
        await unary(req, "updateWorkspace", {
          id: req.params.id,
          ...writeBody(req.body),
        }),
      ),
  );
  app.delete(
    "/api/workspaces/:id",
    {
      preValidation: requireRevisionString,
      schema: {
        params,
        body: envelope(["expected_revision"], { expected_revision: revision }),
      },
    },
    async (req, reply) => {
      checkRevision(req.body.expected_revision);
      await unary(req, "deleteWorkspace", { ...req.params, ...req.body });
      reply.code(204).send();
    },
  );
}
