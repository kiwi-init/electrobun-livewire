import { describe, expect, test } from "bun:test";
import { mkdtempSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";

const GUARD = `if ! grep -qE '[0-9]+ failed, [0-9]+ passed(, [0-9]+ skipped)?' "$RUNNER_TEMP/kitchen-x11.log"; then
  echo "::error::the Kitchen app exited without its result summary; the log may be truncated"
  tail -20 "$RUNNER_TEMP/kitchen-x11.log" || true
  exit 1
fi`;

function guardVerdict(log: string): number {
  const dir = mkdtempSync(join(tmpdir(), "guard-"));
  try {
    writeFileSync(join(dir, "kitchen-x11.log"), log);
    const r = spawnSync("bash", ["-c", `RUNNER_TEMP="${dir}"; set -o pipefail; ${GUARD}`]);
    return r.status ?? 1;
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

describe("the X11 step's missing-summary guard", () => {
  test("a complete run's summary line is accepted, whatever the counts", () => {
    expect(guardVerdict("noise\n14 failed, 170 passed, 20 skipped\n")).toBe(0);
    expect(guardVerdict("noise\n0 failed, 179 passed, 22 skipped\n")).toBe(0);
    expect(guardVerdict("0 failed, 179 passed\n")).toBe(0);
  });

  test("an app exit without the summary is refused", () => {
    expect(guardVerdict("Electrobun Kitchen starting\n[webview:1] Test started: something\n")).toBe(1);
  });

  test("a truncated log that cuts mid-summary is refused", () => {
    expect(guardVerdict("running tests...\n14 failed, 170 pas")).toBe(1);
  });

  test("the workflow carries the guard after the X11 run step", async () => {
    const wf = await Bun.file(new URL("../../../.github/workflows/kitchen-test.yml", import.meta.url).pathname).text();
    const run = wf.indexOf("Run the test (Linux, Xvfb + openbox)");
    const guard = wf.indexOf("without its result summary");
    expect(run).toBeGreaterThan(-1);
    expect(guard).toBeGreaterThan(run);
    expect(wf.indexOf("without its result summary", guard + 1)).toBe(-1);
  });
});
