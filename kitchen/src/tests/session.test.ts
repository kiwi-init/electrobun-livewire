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
        // SameSite=None needs Secure (libsoup refuses it otherwise).
        expect(session.cookies.set({ name: "open", value: "1", domain: "127.0.0.1", path: "/", secure: true, sameSite: "no_restriction" })).toBe(true);

        const stored = session.cookies.get({ url: origin });
        log(`cookies.get({ url }): ${JSON.stringify(stored)}`);
        const byName = (name: string) => stored.find((cookie) => cookie.name === name);
        expect(byName("token")?.httpOnly).toBe(true);
        expect(byName("token")?.sameSite).toBe("strict");
        expect(byName("theme")?.httpOnly).toBe(false);
        expect(byName("theme")?.sameSite).toBe("lax");
        // A secure cookie doesn't match http://; get() with no filter returns every cookie.
        const open = session.cookies.get().find((cookie) => cookie.name === "open");
        expect(open?.sameSite).toBe("no_restriction");
      } finally {
        session.cookies.clear();
      }
    },
  }),

  defineTest({
    name: "cookies.clear removes every cookie in the partition",
    category: "Session",
    description: "After clear(), get() returns nothing for the partition, and other partitions keep theirs",
    timeout: 10000,
    requires: { platform: ["darwin", "linux"] },
    async run({ log }) {
      const cleared = Session.fromPartition(`persist:cookie-clear-${Date.now()}`);
      const kept = Session.fromPartition(`persist:cookie-keep-${Date.now()}`);
      try {
        cleared.cookies.set({ name: "a", value: "1", domain: "127.0.0.1", path: "/" });
        cleared.cookies.set({ name: "b", value: "2", domain: "127.0.0.1", path: "/" });
        kept.cookies.set({ name: "c", value: "3", domain: "127.0.0.1", path: "/" });
        expect(cleared.cookies.get().length).toBe(2);

        cleared.cookies.clear();

        expect(cleared.cookies.get().length).toBe(0);
        expect(kept.cookies.get().map((cookie) => cookie.name)).toEqual(["c"]);
        log("clear() emptied its partition and left the other alone");
      } finally {
        cleared.cookies.clear();
        kept.cookies.clear();
      }
    },
  }),

  defineTest({
    name: "cookies set back to back all reach a page loaded right after (stress)",
    category: "Session",
    description: "Repeats set, set, load with fresh partitions; every load must carry both cookies",
    timeout: 120000,
    requires: { platform: ["darwin", "linux"] },
    async run({ createWindow, log }) {
      const reports: { round: number; header: string; documentCookie: string }[] = [];
      const server = Bun.serve({
        hostname: "127.0.0.1",
        port: 0,
        fetch(request) {
          const url = new URL(request.url);
          if (url.pathname === "/report") {
            reports.push({
              round: Number(url.searchParams.get("round")),
              header: request.headers.get("cookie") ?? "",
              documentCookie: url.searchParams.get("cookie") ?? "",
            });
            return new Response("ok");
          }
          const round = url.searchParams.get("round");
          return new Response(
            `<script>fetch("/report?round=${round}&cookie=" + encodeURIComponent(document.cookie))</script>`,
            { headers: { "Content-Type": "text/html; charset=utf-8", "Cache-Control": "no-store" } },
          );
        },
      });
      const host = await createWindow({ url: "views://test-harness/index.html", title: "Cookie Stress", renderer: "native" });
      const missing: string[] = [];
      try {
        for (let round = 0; round < 12; round++) {
          const partition = `persist:cookie-stress-${Date.now()}-${round}`;
          const session = Session.fromPartition(partition);
          const setToken = session.cookies.set({ name: "token", value: "secret", domain: "127.0.0.1", path: "/", httpOnly: true, sameSite: "strict" });
          const setTheme = session.cookies.set({ name: "theme", value: "dark", domain: "127.0.0.1", path: "/", sameSite: "lax" });
          const view = new BrowserView({ windowId: host.window.id, renderer: "native", partition, url: `http://127.0.0.1:${server.port}/?round=${round}`, frame: { x: 0, y: 0, width: 200, height: 150 } });
          const deadline = Date.now() + 8000;
          while (!reports.some((r) => r.round === round) && Date.now() < deadline) await sleep(100);
          const report = reports.find((r) => r.round === round);
          const header = report?.header ?? "<no request>";
          const ok = header.includes("token=secret") && header.includes("theme=dark");
          log(`round ${round}: set=${setToken}/${setTheme} header="${header}" document.cookie="${report?.documentCookie ?? ""}"`);
          if (!ok) missing.push(`round ${round}: ${header}`);
          view.remove();
          session.cookies.clear();
        }
      } finally {
        server.stop(true);
      }
      expect(missing).toEqual([]);
    },
  }),

  defineTest({
    name: "an httpOnly cookie reaches the server but not document.cookie",
    category: "Session",
    description: "A page in the cookie's partition sends the httpOnly cookie to its server, but its script can't read it",
    timeout: 20000,
    requires: { platform: ["darwin", "linux"] },
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
    name: "ephemeral partitions end with their last view, unless Session holds them",
    category: "Session",
    description: "Views of a non-persist partition share one session while any is alive; after the last is removed a new view starts empty. A partition Session has used keeps its session",
    timeout: 30000,
    requires: { platform: ["darwin", "linux"] },
    async run({ createWindow, log }) {
      const reports = new Map<string, string>();
      const server = Bun.serve({
        hostname: "127.0.0.1",
        port: 0,
        fetch(request) {
          const url = new URL(request.url);
          if (url.pathname === "/report") {
            reports.set(url.searchParams.get("tag") ?? "", url.searchParams.get("cookie") ?? "");
            return new Response("ok");
          }
          const tag = url.searchParams.get("tag") ?? "";
          const set = url.searchParams.has("set") ? `document.cookie = "n=1; path=/";` : "";
          return new Response(
            `<script>${set} fetch("/report?tag=${tag}&cookie=" + encodeURIComponent(document.cookie))</script>`,
            { headers: { "Content-Type": "text/html; charset=utf-8", "Cache-Control": "no-store" } },
          );
        },
      });
      const host = await createWindow({ url: "views://test-harness/index.html", title: "Ephemeral partitions", renderer: "native" });
      const open = (partition: string, query: string) =>
        new BrowserView({ windowId: host.window.id, renderer: "native", partition, url: `http://127.0.0.1:${server.port}/?${query}`, frame: { x: 0, y: 0, width: 300, height: 200 } });
      const reportFor = async (tag: string) => {
        const deadline = Date.now() + 8000;
        while (!reports.has(tag) && Date.now() < deadline) await sleep(100);
        expect(reports.has(tag)).toBe(true);
        return reports.get(tag)!;
      };
      const stamp = Date.now();
      try {
        // Only views hold this one.
        const partition = `ephemeral-refs-${stamp}`;
        const setter = open(partition, "tag=set&set=1");
        expect(await reportFor("set")).toBe("n=1");
        const sibling = open(partition, "tag=sibling");
        expect(await reportFor("sibling")).toBe("n=1");
        setter.remove();
        sibling.remove();
        await sleep(1000);
        const after = await (open(partition, "tag=after"), reportFor("after"));
        log(`views only: set=n=1, sibling=n=1, after last view="${after}"`);
        expect(after).toBe("");

        // Session used this one, so it outlives its views.
        const held = `ephemeral-session-${stamp}`;
        Session.fromPartition(held).cookies.get();
        const heldSetter = open(held, "tag=held-set&set=1");
        expect(await reportFor("held-set")).toBe("n=1");
        heldSetter.remove();
        await sleep(1000);
        const heldAfter = await (open(held, "tag=held-after"), reportFor("held-after"));
        log(`with Session: after last view="${heldAfter}"`);
        expect(heldAfter).toBe("n=1");
      } finally {
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
