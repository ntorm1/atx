import { randomUUID } from "node:crypto";
import { test, expect, type Page } from "@playwright/test";
const base = process.env.ATX_E2E_TEAM_BASE_URL ?? "http://127.0.0.1:3004";
test.use({ baseURL: base });
async function login(page: Page, token: string) {
  await page.goto("/?panel=reviews");
  await page.getByLabel("Workstation access token").fill(token);
  await page.getByRole("button", { name: "Connect", exact: true }).click();
}
test("actual individual credentials preserve actor lineage, deny viewers and require independent review", async ({
  browser,
  page,
  request,
}) => {
  test.skip(
    process.env.ATX_E2E_TEAM_LIVE !== "1",
    "Requires disposable configured native team",
  );
  const researcher = process.env.ATX_E2E_TEAM_RESEARCHER!,
    reviewer = process.env.ATX_E2E_TEAM_REVIEWER!,
    viewer = process.env.ATX_E2E_TEAM_VIEWER!;
  expect(Boolean(researcher && reviewer && viewer)).toBe(true);
  const headers = { Authorization: `Bearer ${researcher}` };
  const alphaResponse = await request.post("/api/alphas", {
    headers,
    data: {
      name: `Team browser alpha ${randomUUID().slice(0, 8)}`,
      project_id: "local",
      description: "Disposable team browser qualification",
      tags: [],
      hypothesis: "Synthetic qualification only",
      request: {
        mode: "synthetic_demo",
        strategy_id: "synthetic-long-short-v1",
        strategy_dsl: "rank(delta(close,5))",
        sessions: 63,
        initial_capital: 1000000,
        trade_bps: 6,
        annual_borrow_bps: 300,
      },
    },
  });
  expect(alphaResponse.status()).toBe(201);
  const alpha = await alphaResponse.json();
  const reviewerContext = await browser.newContext(),
    viewerContext = await browser.newContext();
  try {
    const submitted = await request.post("/api/jobs", {
      headers,
      data: {
        alpha_id: alpha.id,
        alpha_revision: alpha.revision,
        label: "Team native evidence",
        idempotency_key: randomUUID(),
      },
    });
    expect(submitted.status()).toBe(201);
    const job = await submitted.json();
    expect(job.actor_id).toBe("browser-researcher");
    await expect
      .poll(
        async () =>
          (await (await request.get(`/api/jobs/${job.id}`, { headers })).json())
            .status,
      )
      .toBe("succeeded");
    await login(page, researcher);
    await expect(
      page.getByRole("button", { name: "Browser Researcher / researcher" }),
    ).toBeVisible();
    await page.getByRole("button", { name: "Maximize active panel" }).click();
    await page
      .getByLabel("Review alpha", { exact: true })
      .selectOption(alpha.id);
    await page.getByLabel("Team native evidence").check();
    await page
      .getByLabel("Review submission note")
      .fill("Individual actor evidence; synthetic validation only.");
    await page
      .getByRole("button", { name: "Submit review", exact: true })
      .click();
    await expect(
      page.getByText("submitted / r1", { exact: true }),
    ).toBeVisible();
    const reviewRecord = (
      await (
        await request.get(`/api/reviews?alpha_id=${alpha.id}`, { headers })
      ).json()
    ).reviews[0];
    await page
      .getByLabel("Review decision rationale")
      .fill("Requester cannot approve its own evidence.");
    await expect(
      page.getByRole("button", { name: "Approve evidence" }),
    ).toBeDisabled();
    const readOnlyPage = await viewerContext.newPage();
    await login(readOnlyPage, viewer);
    await expect(
      readOnlyPage.getByRole("button", { name: "Browser Viewer / viewer" }),
    ).toBeVisible();
    await expect(
      readOnlyPage.getByRole("button", { name: "▶ Run research" }),
    ).toBeDisabled();
    const denied = await request.post("/api/jobs", {
      headers: { Authorization: `Bearer ${viewer}` },
      data: {
        alpha_id: alpha.id,
        alpha_revision: alpha.revision,
        idempotency_key: randomUUID(),
      },
    });
    expect(denied.status()).toBe(403);
    const reviewPage = await reviewerContext.newPage();
    await login(reviewPage, reviewer);
    await expect(
      reviewPage.getByRole("button", { name: "Browser Reviewer / reviewer" }),
    ).toBeVisible();
    await reviewPage
      .getByRole("button", { name: "Maximize active panel" })
      .click();
    await reviewPage
      .getByRole("cell", { name: "submitted", exact: true })
      .click();
    await reviewPage
      .getByLabel("Review decision rationale")
      .fill(
        "Independent native evidence inspected; no investment certification.",
      );
    await reviewPage.getByRole("button", { name: "Approve evidence" }).click();
    await expect(
      reviewPage.getByText("approved / r2", { exact: true }),
    ).toBeVisible();
    await expect(
      reviewPage.getByRole("cell", { name: "browser-reviewer", exact: true }),
    ).toBeVisible();
    await expect(
      reviewPage.getByRole("button", { name: "Withdraw review" }),
    ).toBeDisabled();
    await reviewPage
      .getByText("Frozen native evidence JSON", { exact: true })
      .click();
    await expect(reviewPage.locator(".review-evidence pre")).toContainText(
      job.id,
    );
    await reviewPage.screenshot({
      path: "artifacts/workstation-team-review-live.png",
    });
    await reviewPage.getByRole("button", { name: "Team", exact: true }).click();
    await expect(
      reviewPage
        .getByRole("row")
        .filter({ hasText: reviewRecord.id.slice(0, 8) })
        .filter({ hasText: "review.approved" }),
    ).toBeVisible();
    await reviewPage
      .getByRole("button", { name: "Maximize active panel" })
      .click();
    await reviewPage.screenshot({
      path: "artifacts/workstation-team-audit-live.png",
    });
    for (const current of [page, readOnlyPage, reviewPage]) {
      const stored = await current.evaluate(() => JSON.stringify(localStorage));
      for (const token of [researcher, reviewer, viewer])
        expect(stored.includes(token)).toBe(false);
    }
  } finally {
    await reviewerContext.close();
    await viewerContext.close();
    await request.post(`/api/alphas/${alpha.id}/archive`, {
      headers,
      data: { expected_revision: alpha.revision, archived: true },
    });
  }
});
