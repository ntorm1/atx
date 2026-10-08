import { test, expect } from "@playwright/test";

test("dense research workstation docks tabs and persists its layout", async ({
  page,
}) => {
  const errors: string[] = [];
  page.on("pageerror", (e) => errors.push(e.message));
  await page.goto("/");
  await expect(
    page.getByText("Research workstation", { exact: true }),
  ).toBeVisible();
  await expect(
    page.getByRole("button", { name: "▶ Run research" }),
  ).toBeVisible();
  await expect(page.getByLabel("Alpha expression")).toHaveValue(
    "rank(delta(close,5))",
  );
  await expect(page.getByLabel("Sessions", { exact: true })).toHaveValue("504");
  expect(
    await page
      .getByLabel("Initial capital")
      .evaluate((input: HTMLInputElement) => input.checkValidity()),
  ).toBe(true);
  await page.screenshot({ path: "artifacts/workstation.png", fullPage: true });
  const groupsBefore = await page.locator(".dv-groupview").count();
  expect(groupsBefore).toBeGreaterThanOrEqual(8);
  const sidebar = await page
    .locator(".dv-groupview")
    .filter({ has: page.locator(".dv-tab").filter({ hasText: "Workspace" }) })
    .boundingBox();
  const configuration = await page
    .locator(".dv-groupview")
    .filter({
      has: page.locator(".dv-tab").filter({ hasText: "Draft configuration" }),
    })
    .boundingBox();
  expect(sidebar!.width).toBeLessThan(240);
  expect(configuration!.width).toBeGreaterThan(300);
  const source = page
    .locator(".dv-tab")
    .filter({ hasText: "Return distribution" });
  const target = page.locator(".dv-tab").filter({ hasText: "Drawdown" });
  await source.dragTo(target);
  await expect
    .poll(() => page.locator(".dv-groupview").count())
    .toBeLessThan(groupsBefore);
  const equity = page.locator(".dv-groupview").filter({
    has: page.locator(".dv-tab").filter({ hasText: "Equity curve" }),
  });
  const equityBox = await equity.boundingBox();
  await source.dragTo(equity, {
    targetPosition: { x: equityBox!.width / 2, y: equityBox!.height - 4 },
  });
  await expect(page.locator(".dv-groupview")).toHaveCount(groupsBefore);
  const saved = await page.evaluate(() =>
    localStorage.getItem("atx.workspace.layout.v2"),
  );
  expect(saved).toBeTruthy();
  const groupsAfter = await page.locator(".dv-groupview").count();
  await page.evaluate(() => {
    const key = "atx.workspace.layout.v2";
    const layout = JSON.parse(localStorage.getItem(key)!);
    layout.panels.configuration.title = "Run configuration";
    layout.panels.editor.title = "Request document";
    localStorage.setItem(key, JSON.stringify(layout));
  });
  await page.reload();
  await expect(page.locator(".dv-groupview")).toHaveCount(groupsAfter);
  await expect(
    page.locator(".dv-tab").filter({ hasText: /^Draft request$/ }),
  ).toBeVisible();
  await page.getByRole("button", { name: "Reset layout", exact: true }).click();
  await expect(page.locator(".dv-groupview")).toHaveCount(groupsBefore);
  await page.getByRole("button", { name: "Windows ▾" }).click();
  await page
    .locator(".dropdown")
    .getByRole("button", { name: "Provenance", exact: true })
    .click();
  await expect(
    page.locator(".dv-tab").filter({ hasText: "Provenance" }),
  ).toHaveClass(/dv-active-tab/);
  expect(errors).toEqual([]);
});

test("compact workspace keeps configuration usable and groups analytical views into tabs", async ({
  page,
}) => {
  await page.setViewportSize({ width: 960, height: 800 });
  await page.goto("/");
  await expect(
    page.getByRole("button", { name: "▶ Run research" }),
  ).toBeVisible();
  await expect(page.getByLabel("Sessions", { exact: true })).toBeVisible();
  await expect(page.locator(".dv-groupview")).toHaveCount(6);
  await page.screenshot({
    path: "artifacts/workstation-compact.png",
    fullPage: true,
  });
});

test("live native engine run renders honest results and scheduled cancellation", async ({
  page,
}) => {
  test.skip(
    process.env.ATX_E2E_LIVE !== "1",
    "Requires a built and running native research server",
  );
  test.setTimeout(60_000);
  await page.goto("/");
  await expect(page.getByText("Connected", { exact: true })).toBeVisible();
  await page.getByLabel("Sessions", { exact: true }).fill("126");
  const expression = "rank(ts_mean(returns,20))";
  await page.getByLabel("Alpha expression").fill(expression);
  const submitted = page.waitForResponse(
    (r) => r.url().endsWith("/api/jobs") && r.request().method() === "POST",
  );
  await page.getByRole("button", { name: "▶ Run research" }).click();
  const response = await submitted;
  expect(response.ok()).toBe(true);
  const created = await response.json();
  const completedRow = page
    .locator("tr.clickable")
    .filter({ has: page.locator(`td[title="${created.id}"]`) });
  await expect(
    completedRow.getByText("succeeded", { exact: true }),
  ).toBeVisible({ timeout: 30_000 });
  await expect(
    page.getByText("SYNTHETIC DEMO — simulated research results", {
      exact: true,
    }),
  ).toBeVisible({ timeout: 30_000 });
  await expect(
    page.getByRole("img", { name: "Net asset value curve" }),
  ).toBeVisible();
  await expect(
    page.getByRole("img", { name: "Distribution of daily returns" }),
  ).toBeVisible();
  const detail = await (
    await page.request.get(`/api/jobs/${created.id}`)
  ).json();
  expect(detail.request.strategy_dsl).toBe(expression);
  expect(detail.result.provenance.synthetic).toBe(true);
  expect(detail.result.provenance.evaluation).toBe("synthetic_test");
  expect(detail.result.equity_curve.length).toBeGreaterThan(100);
  expect(detail.result.trades.length).toBeGreaterThan(0);
  expect(detail.result.metrics.final_nav).toBeGreaterThan(0);
  expect(JSON.stringify(detail.result.provenance)).toContain(expression);
  const drawdown = page.getByRole("img", { name: "Peak to trough drawdown" });
  await expect
    .poll(() =>
      drawdown.evaluate((svg: SVGSVGElement) =>
        Math.abs(svg.viewBox.baseVal.width - svg.getBoundingClientRect().width),
      ),
    )
    .toBeLessThan(1);
  await expect(drawdown.locator("text")).toHaveCount(6);
  const future = new Date(Date.now() + 180_000);
  const local = new Date(future.getTime() - future.getTimezoneOffset() * 60000)
    .toISOString()
    .slice(0, 16);
  await page.getByLabel("Schedule", { exact: true }).fill(local);
  await page.getByRole("button", { name: "Schedule run", exact: true }).click();
  const selected = page.locator("tr.row-selected");
  await expect(
    selected.getByRole("button", { name: "Cancel", exact: true }),
  ).toBeVisible();
  await selected.getByRole("button", { name: "Cancel", exact: true }).click();
  await expect(selected.getByText("cancelled", { exact: true })).toBeVisible();
  await completedRow.click();
  await expect(
    page.getByRole("img", { name: "Net asset value curve" }),
  ).toBeVisible();
  await page
    .locator(".menubar")
    .getByRole("button", { name: "↻ Refresh", exact: true })
    .click();
  await expect(
    page.getByRole("img", { name: "Net asset value curve" }),
  ).toBeVisible();
  await page.reload();
  await expect(
    page.locator("tr.row-selected").getByText("succeeded", { exact: true }),
  ).toBeVisible();
  await page
    .locator(".dv-tab")
    .filter({ hasText: /^Trades$/ })
    .click();
  await expect(
    page.getByRole("columnheader", { name: "instrument", exact: true }),
  ).toBeVisible();
  await page
    .locator(".dv-tab")
    .filter({ hasText: /^Provenance$/ })
    .click();
  await expect(page.getByText(expression, { exact: true })).toBeVisible();
  await page.screenshot({
    path: "artifacts/workstation-live.png",
    fullPage: true,
  });
});

test("request editor validates edits and browser auth stays out of local storage", async ({
  page,
}) => {
  await page.goto("/");
  await page.getByLabel("Research request JSON editor").fill("{broken");
  await page.getByRole("button", { name: "Apply to configuration" }).click();
  await expect(
    page.getByRole("alert").filter({ hasText: /JSON|property|Unexpected/i }),
  ).toBeVisible();
  await page.getByLabel("Research request JSON editor").fill(
    JSON.stringify({
      mode: "synthetic_demo",
      strategy_id: "synthetic-long-short-v1",
      sessions: 126,
      initial_capital: 500000,
      strategy_dsl: "rank(ts_mean(returns,20))",
    }),
  );
  await page.getByRole("button", { name: "Apply to configuration" }).click();
  await expect(page.getByLabel("Sessions", { exact: true })).toHaveValue("126");
  await expect(page.getByLabel("Alpha expression")).toHaveValue(
    "rank(ts_mean(returns,20))",
  );
  await page.getByRole("button", { name: "Access", exact: true }).click();
  await page.getByLabel("Workstation access token").fill("test-browser-token");
  await page.getByRole("button", { name: "Connect", exact: true }).click();
  expect(await page.evaluate(() => JSON.stringify(localStorage))).not.toContain(
    "test-browser-token",
  );
});
