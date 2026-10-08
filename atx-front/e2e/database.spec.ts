import { test, expect } from "@playwright/test";
test.use({ baseURL: "http://127.0.0.1:3002" });
test("database deep link activates the panel without resetting saved groups", async ({
  page,
}) => {
  await page.goto("/?panel=database");
  await expect(
    page.getByRole("table", { name: "Database result" }),
  ).toBeVisible();
  await expect(
    page.locator(".dv-tab").filter({ hasText: /^Database viewer$/ }),
  ).toHaveClass(/dv-active-tab/);
  const layout = await page.evaluate(() =>
    localStorage.getItem("atx.workspace.layout.v2"),
  );
  expect(layout).toBeTruthy();
  const count = await page.locator(".dv-groupview").count();
  await page.reload();
  await expect(page.locator(".dv-groupview")).toHaveCount(count);
  await expect(
    page.getByRole("table", { name: "Database result" }),
  ).toBeVisible();
});
test("wide results keep a bounded DOM and allow scrolling to the last exact row", async ({
  page,
}) => {
  await page.goto("/");
  await page
    .locator(".dv-tab")
    .filter({ hasText: /^Database viewer$/ })
    .click();
  await expect(
    page.getByRole("table", { name: "Database result" }),
  ).toBeVisible();
  await page.getByLabel("Database page size").selectOption("500");
  await page.getByLabel("Database SQL query").fill("SELECT virtual");
  await page.getByRole("button", { name: "Run SELECT", exact: true }).click();
  const table = page.getByRole("table", { name: "Database result" });
  await expect(table).toHaveAttribute("aria-rowcount", "501");
  expect(await table.locator("td").count()).toBeLessThan(20000);
  await page
    .locator(".database-result-scroll")
    .evaluate((element) => (element.scrollTop = element.scrollHeight));
  await expect(
    table.getByRole("cell", { name: "row_499_column_0", exact: true }),
  ).toBeVisible();
  expect(await table.locator("td").count()).toBeLessThan(20000);
  await expect(
    table.getByRole("columnheader", { name: "column_0 VARCHAR", exact: true }),
  ).toBeVisible();
});

test("provider-neutral database browser preserves exact values, NULL, paging and query errors", async ({
  page,
}) => {
  await page.goto("/");
  await page
    .locator(".dv-tab")
    .filter({ hasText: /^Database viewer$/ })
    .click();
  const result = page.getByRole("table", { name: "Database result" });
  await expect(result).toBeVisible();
  await page.getByLabel("Database source").selectOption("fixture-db");
  await page.getByLabel("Database page size").selectOption("200");
  await expect(result).toBeVisible();
  await expect(
    result.getByRole("cell", { name: "9223372036854775807", exact: true }),
  ).toBeVisible();
  await expect(
    result.getByRole("cell", {
      name: "12345678901234567890.123456789012",
      exact: true,
    }),
  ).toBeVisible();
  await expect(result.locator("td.database-null i")).toHaveText("NULL");
  await expect(
    result.getByRole("cell", { name: "NULL", exact: true }),
  ).toHaveCount(2);
  await page.getByRole("button", { name: "Next page", exact: true }).click();
  await expect(page.locator(".database-result-identity")).toContainText(
    "offset 2",
  );
  await expect(
    page.getByRole("button", { name: "Next page", exact: true }),
  ).toBeDisabled();
  await page
    .getByRole("button", { name: "Previous page", exact: true })
    .click();
  await expect(page.locator(".database-result-identity")).toContainText(
    "offset 0",
  );
  await page.getByLabel("Filter database tables").fill("trades");
  await expect(
    page.getByRole("list", { name: "Database tables" }).getByRole("listitem"),
  ).toHaveCount(1);
  await page.getByRole("listitem").filter({ hasText: "trades" }).click();
  await expect(page.locator(".database-result-identity")).toContainText(
    "fixture.main.trades",
  );
  await page.getByRole("listitem").filter({ hasText: "trades" }).click();
  await expect(result).toBeVisible();
  await page
    .getByLabel("Database schema")
    .selectOption(JSON.stringify(["fixture", "audit"]));
  await page.getByLabel("Filter database tables").fill("");
  await expect(page.locator(".database-result-identity")).toContainText(
    "fixture.audit.events",
  );
  const editor = page.getByLabel("Database SQL query");
  await editor.fill("SELECT 9223372036854775807 AS id, NULL AS note");
  await editor.press("Control+Enter");
  await expect(page.locator(".database-result-identity")).toHaveText(
    "Query / Fixture database",
  );
  await expect(page.locator(".database-pagination")).toContainText(
    "more rows available",
  );
  await page.getByLabel("Database page size").selectOption("50");
  await expect(result).toHaveCount(0);
  await page.waitForTimeout(300);
  await expect(result).toHaveCount(0);
  await page.getByRole("button", { name: "Run SELECT", exact: true }).click();
  await expect(page.locator(".database-result-identity")).toHaveText(
    "Query / Fixture database",
  );
  await editor.fill("DELETE FROM fixture.main.prices");
  await expect(result).toHaveCount(0);
  await page.getByRole("button", { name: "Run SELECT", exact: true }).click();
  await expect(page.getByRole("alert")).toContainText(
    "Only read-only SELECT is allowed",
  );
  await expect(result).toHaveCount(0);
  await editor.fill("SELECT delayed");
  await page.getByRole("button", { name: "Run SELECT", exact: true }).click();
  await expect(
    page.getByRole("button", { name: "Cancel request", exact: true }),
  ).toBeVisible();
  await page
    .getByRole("button", { name: "Cancel request", exact: true })
    .click();
  await expect(result).toHaveCount(0);
  await page.getByRole("button", { name: "Run SELECT", exact: true }).click();
  await expect(
    page.getByRole("button", { name: "Cancel request", exact: true }),
  ).toBeVisible();
  await page.getByLabel("Database source").selectOption("archive");
  await expect(page.getByLabel("Database page size")).toHaveValue("25");
  await expect(page.locator(".database-result-identity")).toContainText(
    "fixture.main.prices",
  );
  await page.waitForTimeout(700);
  await expect(page.locator(".database-result-identity")).not.toContainText(
    "Query / Fixture database",
  );
});

test("database viewer splits into an independent dock group and restores its saved position", async ({
  page,
}) => {
  await page.goto("/");
  const source = page
    .locator(".dv-tab")
    .filter({ hasText: /^Database viewer$/ });
  await source.click();
  await expect(
    page.getByRole("table", { name: "Database result" }),
  ).toBeVisible();
  const before = await page.locator(".dv-groupview").count();
  const equity = page.locator(".dv-groupview").filter({
    has: page.locator(".dv-tab").filter({ hasText: /^Equity curve$/ }),
  });
  const bounds = await equity.boundingBox();
  await source.dragTo(equity, {
    targetPosition: { x: bounds!.width / 2, y: bounds!.height - 4 },
  });
  await expect(page.locator(".dv-groupview")).toHaveCount(before + 1);
  await page.reload();
  await expect(page.locator(".dv-groupview")).toHaveCount(before + 1);
  await expect(
    page.locator(".dv-tab").filter({ hasText: /^Database viewer$/ }),
  ).toBeVisible();
});
