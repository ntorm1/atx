import { test, expect, type Page } from "@playwright/test";
test.use({ baseURL: "http://127.0.0.1:3002" });
test.beforeEach(async ({ request }) => {
  await request.post("http://127.0.0.1:3002/fixture/workspaces", {
    data: { reset: true },
  });
});
async function menu(page: Page, action: string) {
  await page.getByRole("button", { name: "Workspace actions" }).click();
  await page.getByRole("button", { name: action, exact: true }).click();
}
async function saveAs(page: Page, name: string) {
  await menu(page, "Save as…");
  await page.getByLabel("Workspace name", { exact: true }).fill(name);
  await page
    .getByRole("button", { name: "Create workspace", exact: true })
    .click();
  await expect(page.locator(".workspace-status")).toHaveText(/r1/);
  return new URL(page.url()).searchParams.get("workspace")!;
}
async function databasePreset(page: Page) {
  await page
    .getByLabel("Workspace", { exact: true })
    .selectOption("template:database");
  const discard = page.getByRole("button", {
    name: "Discard and switch",
    exact: true,
  });
  if (await discard.isVisible()) await discard.click();
  await expect(
    page.getByRole("table", { name: "Database result" }),
  ).toBeVisible();
}
async function config(page: Page) {
  await page
    .locator(".dv-tab")
    .filter({ hasText: /^Draft configuration$/ })
    .click();
  await expect(page.getByLabel("Sessions", { exact: true })).toBeVisible();
}
test("named database workspaces restore geometry, public draft and query context across browsers", async ({
  page,
  browser,
  request,
}) => {
  await page.goto("/");
  await databasePreset(page);
  const pane = await page.locator(".database-panel").boundingBox();
  expect(pane!.width).toBeGreaterThan(800);
  expect(pane!.height).toBeGreaterThan(600);
  await page.getByLabel("Database SQL query").fill("SELECT 'workspace state'");
  await page.getByRole("button", { name: "Run SELECT", exact: true }).click();
  await expect(
    page.getByRole("table", { name: "Database result" }),
  ).toBeVisible();
  await config(page);
  await page.getByLabel("Sessions", { exact: true }).fill("126");
  await page
    .getByLabel("Alpha expression", { exact: true })
    .fill("rank(ts_mean(returns,20))");
  const id = await saveAs(page, "Database desk");
  const stored = await request.get(
    `http://127.0.0.1:3002/api/workspaces/${id}`,
  );
  const record = await stored.json();
  expect(record.document.panel_state.draft_request.sessions).toBe(126);
  expect(record.document.panel_state.database.sql).toBe(
    "SELECT 'workspace state'",
  );
  expect(record.document.panel_state.database.mode).toBe("query");
  expect(JSON.stringify(record.document)).not.toMatch(
    /access_token|authorization|password/,
  );
  const context = await browser.newContext({
    viewport: { width: 1536, height: 1024 },
  });
  const second = await context.newPage();
  await second.goto(`http://127.0.0.1:3002/?workspace=${id}`);
  await expect(second.getByLabel("Sessions", { exact: true })).toHaveValue(
    "126",
  );
  await expect(
    second.getByLabel("Alpha expression", { exact: true }),
  ).toHaveValue("rank(ts_mean(returns,20))");
  await expect(second.locator(".workspace-status")).toHaveText("r1");
  await second
    .locator(".dv-tab")
    .filter({ hasText: /^Database viewer$/ })
    .click();
  await expect(second.getByLabel("Database SQL query")).toHaveValue(
    "SELECT 'workspace state'",
  );
  await expect(
    second.getByRole("table", { name: "Database result" }),
  ).toHaveCount(0);
  await expect(second.locator(".workspace-status")).toContainText("Unsaved");
  await context.close();
});
test("CAS conflict preserves local edits and Save as copy creates a distinct durable record", async ({
  page,
  browser,
  request,
}) => {
  await page.goto("/");
  await page.getByLabel("Sessions", { exact: true }).fill("126");
  const id = await saveAs(page, "Shared desk");
  const context = await browser.newContext();
  const second = await context.newPage();
  await second.goto(`http://127.0.0.1:3002/?workspace=${id}`);
  await expect(second.getByLabel("Sessions", { exact: true })).toHaveValue(
    "126",
  );
  await second.getByLabel("Sessions", { exact: true }).fill("252");
  await second.getByRole("button", { name: "Save", exact: true }).click();
  await expect(second.locator(".workspace-status")).toHaveText(/r2/);
  await page.getByLabel("Sessions", { exact: true }).fill("333");
  await page.getByRole("button", { name: "Save", exact: true }).click();
  await expect(page.getByRole("alert")).toContainText("newer server revision");
  await expect(page.getByLabel("Sessions", { exact: true })).toHaveValue("333");
  await page.getByRole("button", { name: "Save as copy", exact: true }).click();
  await page
    .getByLabel("Workspace name", { exact: true })
    .fill("Shared desk local copy");
  await page
    .getByRole("button", { name: "Create workspace", exact: true })
    .click();
  await expect(page.locator(".workspace-status")).toHaveText(/r1/);
  const copy = new URL(page.url()).searchParams.get("workspace");
  expect(copy).not.toBe(id);
  const original = await (
    await request.get(`http://127.0.0.1:3002/api/workspaces/${id}`)
  ).json();
  expect(original.document.panel_state.draft_request.sessions).toBe(252);
  const duplicated = await (
    await request.get(`http://127.0.0.1:3002/api/workspaces/${copy}`)
  ).json();
  expect(duplicated.document.panel_state.draft_request.sessions).toBe(333);
  await context.close();
});
test("dirty template switches offer explicit choices and metadata rename leaves the draft unsaved", async ({
  page,
  request,
}) => {
  await page.goto("/");
  await page.getByLabel("Sessions", { exact: true }).fill("111");
  await page
    .getByLabel("Workspace", { exact: true })
    .selectOption("template:database");
  await expect(
    page.getByRole("region", { name: "Unsaved workspace changes" }),
  ).toBeVisible();
  await page.getByRole("button", { name: "Keep editing", exact: true }).click();
  await expect(page.getByLabel("Sessions", { exact: true })).toHaveValue("111");
  await page
    .getByLabel("Workspace", { exact: true })
    .selectOption("template:database");
  await page
    .getByRole("button", { name: "Save and switch", exact: true })
    .click();
  await page
    .getByLabel("Workspace name", { exact: true })
    .fill("Research draft");
  await page
    .getByRole("button", { name: "Create workspace", exact: true })
    .click();
  await expect(page.getByLabel("Workspace", { exact: true })).toHaveValue(
    "template:database",
  );
  const list = await (
    await request.get("http://127.0.0.1:3002/api/workspaces")
  ).json();
  const id = list.workspaces[0].id;
  await page
    .getByLabel("Workspace", { exact: true })
    .selectOption(`saved:${id}`);
  await expect(page.getByLabel("Sessions", { exact: true })).toHaveValue("111");
  await page.getByLabel("Sessions", { exact: true }).fill("222");
  await menu(page, "Rename…");
  await page
    .getByLabel("Workspace name", { exact: true })
    .fill("Renamed research");
  await page.getByRole("button", { name: "Apply name", exact: true }).click();
  await expect(page.locator(".workspace-status")).toContainText("Unsaved");
  const renamed = await (
    await request.get(`http://127.0.0.1:3002/api/workspaces/${id}`)
  ).json();
  expect(renamed.name).toBe("Renamed research");
  expect(renamed.document.panel_state.draft_request.sessions).toBe(111);
  await expect(page.getByLabel("Sessions", { exact: true })).toHaveValue("222");
  await menu(page, "Delete…");
  await page
    .getByRole("button", { name: "Delete saved workspace", exact: true })
    .click();
  await expect(page.getByLabel("Workspace", { exact: true })).toHaveValue(
    "local",
  );
  await expect(page.getByLabel("Sessions", { exact: true })).toHaveValue("222");
  expect(
    (await request.get(`http://127.0.0.1:3002/api/workspaces/${id}`)).status(),
  ).toBe(404);
});
test("edits made during asynchronous save or load are retained and remain unsaved", async ({
  page,
  request,
}) => {
  await page.goto("/");
  const id = await saveAs(page, "Slow desk");
  await request.post("http://127.0.0.1:3002/fixture/workspaces", {
    data: { save_delay_ms: 600 },
  });
  await page.getByLabel("Sessions", { exact: true }).fill("126");
  const saving = page.waitForResponse(
    (response) =>
      response.request().method() === "PUT" &&
      response.url().includes(`/api/workspaces/${id}`),
  );
  await page.getByRole("button", { name: "Save", exact: true }).click();
  await page.getByLabel("Sessions", { exact: true }).fill("252");
  await saving;
  await expect(page.getByRole("alert")).toContainText(
    "Newer edits are still unsaved",
  );
  await expect(page.getByLabel("Sessions", { exact: true })).toHaveValue("252");
  const record = await (
    await request.get(`http://127.0.0.1:3002/api/workspaces/${id}`)
  ).json();
  expect(record.document.panel_state.draft_request.sessions).toBe(126);
  await request.post("http://127.0.0.1:3002/fixture/workspaces", {
    data: { load_delay_ms: 600 },
  });
  await menu(page, "Reload saved");
  await page
    .getByRole("button", { name: "Discard and switch", exact: true })
    .click();
  await page.getByLabel("Sessions", { exact: true }).fill("333");
  await expect(page.getByRole("alert")).toContainText("while loading");
  await expect(page.getByLabel("Sessions", { exact: true })).toHaveValue("333");
  await expect(
    page.getByRole("region", { name: "Unsaved workspace changes" }),
  ).toBeVisible();
});
test("legacy geometry remains local and malformed saved layouts cannot replace working state", async ({
  page,
  request,
}) => {
  await page.goto("/?panel=database");
  await expect(
    page.getByRole("table", { name: "Database result" }),
  ).toBeVisible();
  const groups = await page.locator(".dv-groupview").count();
  await page.reload();
  await expect(page.locator(".dv-groupview")).toHaveCount(groups);
  await expect(page.getByLabel("Workspace", { exact: true })).toHaveValue(
    "local",
  );
  expect(
    (await (await request.get("http://127.0.0.1:3002/api/workspaces")).json())
      .total,
  ).toBe("0");
  await config(page);
  const id = await saveAs(page, "Working geometry");
  const record = await (
    await request.get(`http://127.0.0.1:3002/api/workspaces/${id}`)
  ).json();
  const malformed = await (
    await request.post("http://127.0.0.1:3002/api/workspaces", {
      data: {
        name: "Malformed geometry",
        document: {
          ...record.document,
          layout: { ...record.document.layout, grid: {} },
        },
      },
    })
  ).json();
  await menu(page, "Refresh workspace list");
  await expect(
    page
      .getByLabel("Workspace", { exact: true })
      .locator(`option[value="saved:${malformed.id}"]`),
  ).toHaveCount(1);
  await page.getByLabel("Sessions", { exact: true }).fill("333");
  await page
    .getByLabel("Workspace", { exact: true })
    .selectOption(`saved:${malformed.id}`);
  await page
    .getByRole("button", { name: "Discard and switch", exact: true })
    .click();
  await expect(page.locator(".workspace-error")).toBeVisible();
  await expect(page.getByLabel("Sessions", { exact: true })).toHaveValue("333");
  await expect(page.getByLabel("Workspace", { exact: true })).toHaveValue(
    `saved:${id}`,
  );
  await expect(page.locator(".dv-groupview")).toHaveCount(groups);
});
test("unavailable saved database sources retain SQL and show honest errors", async ({
  page,
  request,
}) => {
  await page.goto("/");
  await databasePreset(page);
  const id = await saveAs(page, "Source desk");
  const record = await (
    await request.get(`http://127.0.0.1:3002/api/workspaces/${id}`)
  ).json();
  const missing = await (
    await request.post("http://127.0.0.1:3002/api/workspaces", {
      data: {
        name: "Missing source desk",
        document: {
          ...record.document,
          panel_state: {
            ...record.document.panel_state,
            database: {
              ...record.document.panel_state.database,
              source_id: "removed-source",
              sql: "SELECT 'keep this SQL'",
              mode: "query",
            },
          },
        },
      },
    })
  ).json();
  await page.goto(`/?workspace=${missing.id}`);
  await expect(page.getByLabel("Database SQL query")).toHaveValue(
    "SELECT 'keep this SQL'",
  );
  await expect(
    page.locator(".database-panel").getByRole("alert"),
  ).toContainText("unavailable");
  await expect(
    page.getByRole("table", { name: "Database result" }),
  ).toHaveCount(0);
  await expect(page.locator(".workspace-status")).toHaveText("r1");
});
test("unapplied JSON edits prompt before switching and cannot be falsely saved", async ({
  page,
  request,
}) => {
  await page.goto("/");
  const id = await saveAs(page, "Editor desk");
  await page
    .locator(".dv-tab")
    .filter({ hasText: /^Draft request$/ })
    .click();
  const editor = page.getByLabel("Research request JSON editor");
  const text = JSON.stringify({
    mode: "synthetic_demo",
    strategy_id: "synthetic-long-short-v1",
    sessions: 222,
  });
  await editor.fill(text);
  await page
    .getByLabel("Workspace", { exact: true })
    .selectOption("template:database");
  await expect(
    page.getByRole("region", { name: "Unsaved workspace changes" }),
  ).toBeVisible();
  await page.getByRole("button", { name: "Keep editing", exact: true }).click();
  await page.getByRole("button", { name: "Save", exact: true }).click();
  await expect(page.locator(".workspace-error")).toContainText(
    "Apply or revert",
  );
  await expect(editor).toHaveValue(text);
  const before = await (
    await request.get(`http://127.0.0.1:3002/api/workspaces/${id}`)
  ).json();
  expect(before.revision).toBe("1");
  await page
    .getByRole("button", { name: "Apply to configuration", exact: true })
    .click();
  await page.getByRole("button", { name: "Save", exact: true }).click();
  await expect(page.locator(".workspace-status")).toHaveText(/r2/);
  const after = await (
    await request.get(`http://127.0.0.1:3002/api/workspaces/${id}`)
  ).json();
  expect(after.document.panel_state.draft_request.sessions).toBe(222);
});
