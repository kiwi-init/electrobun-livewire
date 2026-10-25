// Session/Cookie API Tests

import { defineTest, expect, sleep } from "../test-framework/types";
import { BrowserView, Session } from "electrobun/main";

export const sessionTests = [
  defineTest({
    name: "cookies.set honours httpOnly and sameSite",
    category: "Session",
    description: "httpOnly and sameSite set from the main process round-trip through cookies.get()",
    timeout: 10000,
    requires: { platform: ["darwin", "linux"] },
    async run({ log }) {
      const origin = "http://127.0.0.1:8080";
      const session = Session.fromPartition(`persist:cookie-flags-${Date.now()}`);
      try {
        expect(session.cookies.set({ name: "token", value: "secret", domain: "127.0.0.1", path: "/", httpOnly: true, sameSite: "strict" })).toBe(true);
        expect(session.cookies.set({ name: "theme", value: "dark", domain: "127.0.0.1", path: "/", sameSite: "lax" })).toBe(true);
        expect(session.cookies.set({ name: "open", value: "1", domain: "127.0.0.1", path: "/", sameSite: "no_restriction" })).toBe(true);

        const stored = session.cookies.get({ url: origin });
        log(`cookies.get({ url }): ${JSON.stringify(stored)}`);
        const byName = (name: string) => stored.find((cookie) => cookie.name === name);
        expect(byName("token")?.httpOnly).toBe(true);
        expect(byName("token")?.sameSite).toBe("strict");
        expect(byName("theme")?.httpOnly).toBe(false);
        expect(byName("theme")?.sameSite).toBe("lax");
        expect(byName("open")?.sameSite).toBe("no_restriction");
      } finally {
        session.cookies.clear();
      }
    },
  }),

  defineTest({
    name: "an httpOnly cookie reaches the server but not document.cookie",
    category: "Session",
    description: "A page in the cookie's partition sends the httpOnly cookie to its server, but its script can't read it",
    timeout: 20000,
    // Linux: cookies set through Session don't reach views of the same
    // partition yet (separate data managers), tracked on its own.
    requires: { platform: "darwin" },
    async run({ createWindow, log }) {
      const reports: { documentCookie: string; header: string }[] = [];
      const server = Bun.serve({
        hostname: "127.0.0.1",
        port: 0,
        fetch(request) {
          const url = new URL(request.url);
          if (url.pathname === "/report") {
            reports.push({ documentCookie: url.searchParams.get("cookie") ?? "", header: request.headers.get("cookie") ?? "" });
            return new Response("ok");
          }
          return new Response(
            `<script>fetch("/report?cookie=" + encodeURIComponent(document.cookie))</script>`,
            { headers: { "Content-Type": "text/html; charset=utf-8", "Cache-Control": "no-store" } },
          );
        },
      });
      const partition = `persist:cookie-page-${Date.now()}`;
      const session = Session.fromPartition(partition);
      try {
        session.cookies.set({ name: "token", value: "secret", domain: "127.0.0.1", path: "/", httpOnly: true, sameSite: "strict" });
        session.cookies.set({ name: "theme", value: "dark", domain: "127.0.0.1", path: "/", sameSite: "lax" });

        const host = await createWindow({ url: "views://test-harness/index.html", title: "Cookie Flags", renderer: "native" });
        new BrowserView({ windowId: host.window.id, renderer: "native", partition, url: `http://127.0.0.1:${server.port}/`, frame: { x: 0, y: 0, width: 400, height: 300 } });
        const deadline = Date.now() + 8000;
        while (reports.length === 0 && Date.now() < deadline) await sleep(200);
        log(`page report: ${JSON.stringify(reports[0])}`);
        expect(reports.length > 0).toBe(true);
        expect(reports[0]!.documentCookie).toBe("theme=dark");
        expect(reports[0]!.header).toContain("token=secret");
      } finally {
        session.cookies.clear();
        server.stop(true);
      }
    },
  }),

  defineTest({
    name: "Session.fromPartition",
    category: "Session",
    description: "Test creating a session from a partition name",
    async run({ log }) {
      const session = Session.fromPartition("persist:test-partition");

      expect(session).toBeTruthy();
      expect(session.partition).toBe("persist:test-partition");

      log(`Created session for partition: ${session.partition}`);
    },
  }),

  defineTest({
    name: "Session.defaultSession",
    category: "Session",
    description: "Test accessing the default session",
    async run({ log }) {
      const session = Session.defaultSession;

      expect(session).toBeTruthy();
      expect(session.partition).toBeTruthy();

      log(`Default session partition: ${session.partition}`);
    },
  }),

  defineTest({
    name: "cookies API exists",
    category: "Session",
    description: "Test that cookies API methods exist",
    async run({ log }) {
      const session = Session.fromPartition("persist:cookie-api-test");

      // Test that all cookie methods exist
      expect(typeof session.cookies.set).toBe("function");
      expect(typeof session.cookies.get).toBe("function");
      expect(typeof session.cookies.remove).toBe("function");
      expect(typeof session.cookies.clear).toBe("function");

      log("All cookies API methods exist");
    },
  }),

  // DISABLED: Causes AVX crash in ARM Windows VM
  // defineTest({
  //   name: "cookies.set call",
  //   category: "Session",
  //   description: "Test calling cookies.set without error",
  //   async run({ log }) {
  //     const session = Session.fromPartition("persist:cookie-set-test");

  //     log("Setting test cookie");
  //     // Just test that the call doesn't throw
  //     const result = session.cookies.set({
  //       name: "test-cookie",
  //       value: "test-value-123",
  //       domain: "localhost",
  //       path: "/",
  //       secure: false,
  //       httpOnly: false,
  //       sameSite: "lax",
  //       expirationDate: Math.floor(Date.now() / 1000) + 3600,
  //     });

  //     // We expect this to return something (true/false or a result)
  //     log(`cookies.set returned: ${result}`);
  //   },
  // }),

  // DISABLED: Causes AVX crash in ARM Windows VM
  // defineTest({
  //   name: "cookies.get call",
  //   category: "Session",
  //   description: "Test calling cookies.get without error",
  //   async run({ log }) {
  //     const session = Session.fromPartition("persist:cookie-get-test");

  //     log("Getting all cookies");
  //     const allCookies = session.cookies.get();
  //     expect(Array.isArray(allCookies)).toBe(true);

  //     log(`cookies.get returned ${allCookies.length} cookies`);
  //   },
  // }),

  // DISABLED: Causes AVX crash in ARM Windows VM
  // defineTest({
  //   name: "cookies.clear call",
  //   category: "Session",
  //   description: "Test calling cookies.clear without error",
  //   async run({ log }) {
  //     const session = Session.fromPartition("persist:cookie-clear-test");

  //     log("Clearing all cookies");
  //     // Just verify this doesn't throw
  //     session.cookies.clear();

  //     log("cookies.clear completed without error");
  //   },
  // }),
];
