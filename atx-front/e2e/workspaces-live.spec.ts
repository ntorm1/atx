import { test, expect } from "@playwright/test";
test.skip(
  process.env.ATX_E2E_WORKSPACES_LIVE !== "1",
  "Requires native workspace CRUD",
);
test("native named database desk persists across browsers and rejects conflicting revisions", async ({
  page,
  browser,
  request,
  baseURL,
}) => {
  const name = `Browser qualification ${Date.now()}`;
  const ids: string[] = [];
  try {
    await page.goto("/");
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
    await page
      .getByLabel("Database SQL query")
      .fill(
        "SELECT CAST(9223372036854775807 AS BIGINT) AS exact_integer, NULL AS missing",
      );
    await page.getByRole("button", { name: "Run SELECT", exact: true }).click();
    await expect(
      page.getByRole("cell", { name: "9223372036854775807", exact: true }),
    ).toBeVisible();
    await page
      .locator(".dv-tab")
      .filter({ hasText: /^Draft configuration$/ })
      .click();
    await page.getByLabel("Sessions", { exact: true }).fill("126");
    await page.getByRole("button", { name: "Workspace actions" }).click();
    await page.getByRole("button", { name: "Save as…", exact: true }).click();
    await page.getByLabel("Workspace name", { exact: true }).fill(name);
    await page
      .getByRole("button", { name: "Create workspace", exact: true })
      .click();
    await expect(page.locator(".workspace-status")).toHaveText("r1");
    const id = new URL(page.url()).searchParams.get("workspace")!;
    ids.push(id);
    const record = await (await request.get(`/api/workspaces/${id}`)).json();
    expect(record.document.panel_state.draft_request.sessions).toBe(126);
    expect(record.document.panel_state.database.mode).toBe("query");
    const context = await browser.newContext({
      viewport: { width: 1536, height: 1024 },
    });
    const second = await context.newPage();
    await second.goto(`${baseURL}/?workspace=${id}`);
    await expect(second.getByLabel("Sessions", { exact: true })).toHaveValue(
      "126",
    );
    await expect(second.locator(".workspace-status")).toHaveText("r1");
    await second.getByLabel("Sessions", { exact: true }).fill("252");
    await second.getByRole("button", { name: "Save", exact: true }).click();
    await expect(second.locator(".workspace-status")).toHaveText(/r2/);
    await page.getByLabel("Sessions", { exact: true }).fill("333");
    await page.getByRole("button", { name: "Save", exact: true }).click();
    await expect(page.getByRole("alert")).toContainText(
      "newer server revision",
    );
    await expect(page.getByLabel("Sessions", { exact: true })).toHaveValue(
      "333",
    );
    await second
      .locator(".dv-tab")
      .filter({ hasText: /^Database viewer$/ })
      .click();
    await expect(second.getByLabel("Database SQL query")).toContainText(
      "exact_integer",
    );
    await expect(
      second.getByRole("table", { name: "Database result" }),
    ).toHaveCount(0);
    await second
      .getByRole("button", { name: "Run SELECT", exact: true })
      .click();
    await expect(
      second.getByRole("cell", { name: "9223372036854775807", exact: true }),
    ).toBeVisible();
    await second.screenshot({
      path: "artifacts/workstation-named-database-live.png",
      fullPage: true,
    });
    await context.close();
  } finally {
    for (const id of ids) {
      const response = await request.get(`/api/workspaces/${id}`);
      if (response.ok()) {
        const record = await response.json();
        await request.delete(`/api/workspaces/${id}`, {
          data: { expected_revision: record.revision },
        });
      }
    }
  }
});
