import { defineTest, expect, sleep } from "../test-framework/types";
import { Tray, Utils } from "electrobun/main";



export const trayApiTests = [
  defineTest({
    name: "Tray visibility toggle and bounds",
    category: "Tray",
    description: "Test tray.setVisible and tray.getBounds API behavior",
    timeout: 15000,
    async run({ log }) {
      const tray = new Tray({
        title: "Kitchen Tray API Test",
        image: "views://assets/electrobun-logo-32-template.png",
        template: true,
        width: 32,
        height: 32,
      });

      try {
        tray.setMenu([{ type: "normal", label: "Ping", action: "ping" }]);
        tray.setVisible(false);
        await sleep(100);
        tray.setVisible(true);
        await sleep(100);

        const bounds = tray.getBounds();
        expect(typeof bounds.x).toBe("number");
        expect(typeof bounds.y).toBe("number");
        expect(typeof bounds.width).toBe("number");
        expect(typeof bounds.height).toBe("number");
        log(
          `Tray bounds returned: x=${bounds.x}, y=${bounds.y}, width=${bounds.width}, height=${bounds.height}`,
        );
      } finally {
        tray.remove();
      }
    },
  }),

  defineTest({
    name: "Tray host availability",
    category: "Tray",
    description: "Tray.hostAvailable is true on macOS and Windows, a boolean on Linux",
    timeout: 5000,
    async run({ log }) {
      const available = Tray.hostAvailable();
      expect(typeof available).toBe("boolean");
      if (process.platform !== "linux") {
        expect(available).toBe(true);
      }
      log(`Tray host available: ${available}`);
    },
  }),

  defineTest({
    name: "Dock icon visibility contract",
    category: "Utils",
    description: "Test Utils.setDockIconVisible and Utils.isDockIconVisible",
    timeout: 12000,
    async run({ log }) {
      const initialVisible = Utils.isDockIconVisible();
      expect(typeof initialVisible).toBe("boolean");
      log(`Initial dock icon visibility: ${initialVisible}`);

      try {
        if (process.platform === "darwin") {
          Utils.setDockIconVisible(false);
          await sleep(200);
          expect(Utils.isDockIconVisible()).toBe(false);

          Utils.setDockIconVisible(true);
          await sleep(200);
          expect(Utils.isDockIconVisible()).toBe(true);
          log("Dock icon visibility toggled successfully on macOS");
        } else {
          Utils.setDockIconVisible(false);
          await sleep(50);
          const afterToggle = Utils.isDockIconVisible();
          expect(typeof afterToggle).toBe("boolean");
          log(
            `Dock icon APIs are callable on ${process.platform} (stub behavior expected)`,
          );
        }
      } finally {
        Utils.setDockIconVisible(initialVisible);
      }
    },
  }),

  defineTest({
    name: "Dock badge contract",
    category: "Utils",
    description: "Test Utils.setDockBadge and Utils.getDockBadge with counts, text and clearing",
    timeout: 12000,
    async run({ log }) {
      try {
        if (process.platform === "darwin") {
          Utils.setDockBadge(7);
          expect(Utils.getDockBadge()).toBe("7");
          Utils.setDockBadge("new");
          expect(Utils.getDockBadge()).toBe("new");
          Utils.setDockBadge(0);
          expect(Utils.getDockBadge()).toBe(null);
          Utils.setDockBadge("99+");
          Utils.setDockBadge(null);
          expect(Utils.getDockBadge()).toBe(null);
          log("Dock badge set, changed and cleared on macOS");
        } else {
          Utils.setDockBadge(3);
          expect(Utils.getDockBadge()).toBe(null);
          log(`Dock badge APIs are callable on ${process.platform} (stub behavior expected)`);
        }
      } finally {
        Utils.setDockBadge(null);
      }
    },
  }),

  defineTest({
    name: "Dock progress contract",
    category: "Utils",
    description: "Test Utils.setDockProgress and Utils.getDockProgress, including clamping and removal",
    timeout: 12000,
    async run({ log }) {
      try {
        if (process.platform === "darwin") {
          expect(Utils.getDockProgress()).toBe(null);
          Utils.setDockProgress(0.4);
          expect(Utils.getDockProgress()).toBe(0.4);
          Utils.setDockProgress(2);
          expect(Utils.getDockProgress()).toBe(1);
          Utils.setDockProgress(null);
          expect(Utils.getDockProgress()).toBe(null);
          log("Dock progress shown, clamped and removed on macOS");
        } else {
          Utils.setDockProgress(0.5);
          expect(Utils.getDockProgress()).toBe(null);
          log(`Dock progress APIs are callable on ${process.platform} (stub behavior expected)`);
        }
      } finally {
        Utils.setDockProgress(null);
      }
    },
  }),

  defineTest({
    name: "Notification with actions and reply is accepted",
    category: "Utils",
    description: "Utils.showNotification takes actions, a reply field and an id, and returns that id (delivery and responses need a manual check)",
    timeout: 12000,
    async run({ log }) {
      const id = Utils.showNotification({
        id: "kitchen-actions",
        title: "Kitchen",
        body: "Notification with actions",
        silent: true,
        actions: [{ id: "open", title: "Open", foreground: true }, { id: "delete", title: "Delete", destructive: true }],
        reply: { placeholder: "Reply" },
      });
      expect(id).toBe("kitchen-actions");
      expect(typeof Utils.showNotification({ title: "Kitchen", silent: true })).toBe("string");
      log(`showNotification returned ${id}`);
    },
  }),
];
