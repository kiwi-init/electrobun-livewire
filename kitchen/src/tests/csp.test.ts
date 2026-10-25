// CSP Tests - contentSecurityPolicy is sent as a header on views:// responses

import { defineTest, expect, sleep } from "../test-framework/types";
import { createTestHarnessRPC } from "./rpc.test";

const IMAGE_PROBE = `
  return new Promise((resolve) => {
    const violations = [];
    document.addEventListener("securitypolicyviolation", (e) => violations.push(e.effectiveDirective || e.violatedDirective));
    const img = new Image();
    img.onload = () => resolve({ loaded: true, violations });
    img.onerror = () => setTimeout(() => resolve({ loaded: false, violations }), 200);
    img.src = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";
  });
`;

async function openHarness(createWindow: any, contentSecurityPolicy?: string) {
  const rpc = createTestHarnessRPC(5000);
  const win = await createWindow({
    url: "views://test-harness/index.html",
    rpc,
    renderer: "native",
    title: "CSP Test",
    contentSecurityPolicy,
  });
  await sleep(1500);
  return { rpc, win };
}

export const cspTests = [
  defineTest({
    name: "contentSecurityPolicy is sent on views:// responses",
    category: "CSP",
    description: "A view's contentSecurityPolicy reaches its views:// page as a header, and setContentSecurityPolicy(null) removes it on reload",
    timeout: 30000,
    requires: { platform: ["darwin", "linux"], renderer: "native" },
    async run({ createWindow, log }) {
      const control = await openHarness(createWindow);
      const open = await control.rpc.request.evaluateJavascriptWithResponse({ script: IMAGE_PROBE });
      log(`no policy: ${JSON.stringify(open)}`);
      expect(open.loaded).toBe(true);

      const policy = "default-src * data: blob: 'unsafe-inline' 'unsafe-eval'; img-src 'none'";
      const { rpc, win } = await openHarness(createWindow, policy);
      const blocked = await rpc.request.evaluateJavascriptWithResponse({ script: IMAGE_PROBE });
      log(`img-src 'none': ${JSON.stringify(blocked)}`);
      expect(blocked.loaded).toBe(false);
      expect(blocked.violations).toContain("img-src");

      win.webview.setContentSecurityPolicy(null);
      win.webview.loadURL("views://test-harness/index.html");
      await sleep(1500);
      const cleared = await rpc.request.evaluateJavascriptWithResponse({ script: IMAGE_PROBE });
      log(`after setContentSecurityPolicy(null) and a reload: ${JSON.stringify(cleared)}`);
      expect(cleared.loaded).toBe(true);
    },
  }),
];
