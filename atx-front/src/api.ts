import type { WorkspaceEvent } from "./types";
export class ApiError extends Error {
  constructor(
    message: string,
    public status: number,
  ) {
    super(message);
  }
}
export async function request<T>(
  path: string,
  token: string,
  init: RequestInit = {},
): Promise<T> {
  const response = await fetch(`/api${path}`, {
    ...init,
    signal: init.signal ?? AbortSignal.timeout(20_000),
    headers: {
      ...(init.body ? { "Content-Type": "application/json" } : {}),
      ...(token ? { Authorization: `Bearer ${token}` } : {}),
      ...init.headers,
    },
  });
  if (response.status === 204) return undefined as T;
  const body = await response.json();
  if (!response.ok)
    throw new ApiError(
      body.error ?? `Request failed (${response.status})`,
      response.status,
    );
  return body as T;
}
export async function events(
  token: string,
  after: string,
  signal: AbortSignal,
  receive: (event: WorkspaceEvent) => void,
  connected?: () => void,
  interrupted?: () => void,
): Promise<void> {
  const response = await fetch(
    `/api/events?channel=all&after_sequence=${encodeURIComponent(after)}`,
    { headers: token ? { Authorization: `Bearer ${token}` } : {}, signal },
  );
  if (!response.ok || !response.body)
    throw new ApiError("Event connection unavailable", response.status);
  connected?.();
  const reader = response.body.getReader();
  const decoder = new TextDecoder();
  let buffer = "";
  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      buffer += decoder.decode(value, { stream: true });
      if (buffer.length > 2 * 1024 * 1024)
        throw new Error("Event message exceeds limit");
      let index: number;
      while ((index = buffer.indexOf("\n\n")) >= 0) {
        const block = buffer.slice(0, index);
        buffer = buffer.slice(index + 2);
        const data = block
          .split("\n")
          .filter((line) => line.startsWith("data:"))
          .map((line) => line.slice(5).trim())
          .join("\n");
        if (data && block.includes("event: workspace"))
          receive(JSON.parse(data));
        if (block.includes("event: fault"))
          throw new Error("Native event stream interrupted");
      }
    }
  } finally {
    await reader.cancel().catch(() => undefined);
    if (!signal.aborted) interrupted?.();
  }
}
