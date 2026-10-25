// Utils Tests - Tests for utility functions (clipboard, notifications, etc.)

import { defineTest, expect, sleep } from "../test-framework/types";
import Electrobun, { Utils } from "electrobun/main";
import { mkdir, mkdtemp, writeFile, access } from "fs/promises";
import { homedir, tmpdir } from "os";
import { spawnSync } from "child_process";
import { join } from "path";

export const utilsTests = [
  // Note: Clipboard tests that require user interaction are in clipboard-interactive.test.ts

  defineTest({
    name: "clipboardWriteText and clipboardReadText",
    category: "Utils",
    description: "Test writing and reading text from clipboard",
    async run({ log }) {
      const testText = `Test clipboard ${Date.now()}`;

      log("Writing to clipboard");
      Utils.clipboardWriteText(testText);

      log("Reading from clipboard");
      const read = Utils.clipboardReadText();

      expect(read).toBe(testText);
      log(`Clipboard round-trip successful: "${read}"`);
    },
  }),

  defineTest({
    name: "Clipboard is shared with other Wayland clients",
    category: "Utils",
    description: "Text the app copies is what wl-paste reads, and text wl-copy sets is what the app reads",
    requires: { platform: "linux", display: "wayland" },
    timeout: 15000,
    async run({ log }) {
      const wlCopy = Bun.which("wl-copy");
      const wlPaste = Bun.which("wl-paste");
      if (!wlCopy || !wlPaste) {
        throw new Error("wl-copy and wl-paste are needed (install wl-clipboard)");
      }

      // A compositor only takes a selection from a client with an input
      // event newer than the current selection. A headless compositor on CI has no
      // keyboard or pointer, so press Shift on a virtual keyboard, as a real
      // click or keypress would.
      const giveAppAnInputEvent = async () => {
        if (!process.env["KITCHEN_HEADLESS_WAYLAND"]) return;
        const wtype = Bun.which("wtype");
        if (!wtype) throw new Error("wtype is needed under a headless compositor");
        Bun.spawnSync([wtype, "-k", "Shift_L"], { timeout: 5000 });
        await sleep(100);
      };

      await giveAppAnInputEvent();
      const fromApp = `from Electrobun ${Date.now()}`;
      Utils.clipboardWriteText(fromApp);
      await sleep(200);
      const types = Bun.spawnSync([wlPaste, "--list-types"], { timeout: 5000 });
      log(`wl-paste --list-types: ${types.stdout.toString().trim() || types.stderr.toString().trim()}`);
      const pasted = Bun.spawnSync([wlPaste, "--no-newline"], { timeout: 5000 });
      expect(pasted.stdout.toString()).toBe(fromApp);
      log(`wl-paste read the app's text: "${fromApp}"`);

      const fromOther = `from wl-copy ${Date.now()}`;
      const copied = Bun.spawnSync([wlCopy, fromOther], { timeout: 5000 });
      expect(copied.exitCode).toBe(0);
      await sleep(300);
      expect(Utils.clipboardReadText()).toBe(fromOther);
      log(`The app read wl-copy's text: "${fromOther}"`);

      // Hand the clipboard back to the app, so later clipboard tests aren't
      // reading wl-copy's selection.
      await giveAppAnInputEvent();
      Utils.clipboardWriteText(fromApp);
      await sleep(200);
    },
  }),

  defineTest({
    name: "clipboardAvailableFormats",
    category: "Utils",
    description: "Test getting available clipboard formats",
    async run({ log }) {
      // First write some text
      Utils.clipboardWriteText("test");

      log("Getting available formats");
      const formats = Utils.clipboardAvailableFormats();

      expect(Array.isArray(formats)).toBe(true);
      log(`Available formats: ${formats.join(", ")}`);
    },
  }),

  defineTest({
    name: "clipboardClear",
    category: "Utils",
    description: "Test clearing the clipboard",
    async run({ log }) {
      // First write some text
      Utils.clipboardWriteText("text to clear");

      log("Clearing clipboard");
      Utils.clipboardClear();

      const text = Utils.clipboardReadText();
      // After clear, should be empty or null
      expect(!text || text === "").toBe(true);
      log("Clipboard cleared successfully");
    },
  }),

  defineTest({
    name: "showNotification",
    category: "Utils",
    description: "Test showing a desktop notification",
    async run({ log }) {
      log("Showing notification");
      Utils.showNotification({
        title: "Test Notification",
        body: "This is a test notification from the integration tests",
        subtitle: "Electrobun Tests",
        silent: true, // Don't make sound during tests
      });

      // Give notification time to show
      await sleep(500);
      log("Notification sent (verify visually if needed)");
    },
  }),

  defineTest({
    name: "openExternal",
    category: "Utils",
    description: "Test opening external URL (skipped to avoid opening browser)",
    async run({ log }) {
      // We skip actually calling this to avoid opening the browser during tests
      // But we verify the function exists
      expect(typeof Utils.openExternal).toBe("function");
      log("openExternal function exists (skipped actual call)");
    },
  }),

  defineTest({
    name: "openPath",
    category: "Utils",
    description: "Test opening path (skipped to avoid opening finder)",
    async run({ log }) {
      // We skip actually calling this to avoid side effects
      expect(typeof Utils.openPath).toBe("function");
      log("openPath function exists (skipped actual call)");
    },
  }),

  defineTest({
    name: "showItemInFolder",
    category: "Utils",
    description: "Test showing item in folder (skipped to avoid opening finder)",
    async run({ log }) {
      expect(typeof Utils.showItemInFolder).toBe("function");
      log("showItemInFolder function exists (skipped actual call)");
    },
  }),

  defineTest({
    name: "moveToTrash",
    category: "Utils",
    description: "Test moving a file to trash",
    async run({ log }) {
      // Use the app's data filesystem: Linux does not support trashing files
      // from system-internal mounts such as /tmp.
      await mkdir(Utils.paths.userData, { recursive: true });
      const tempDir = await mkdtemp(
        join(Utils.paths.userData, "electrobun-test-"),
      );
      const testFile = join(tempDir, "test-trash-file.txt");

      log(`Creating temp file: ${testFile}`);
      await writeFile(testFile, "This file will be moved to trash");

      // Verify file exists
      try {
        await access(testFile);
        log("File created successfully");
      } catch {
        throw new Error("Failed to create temp file");
      }

      // Move to trash
      log("Moving file to trash");
      const result = Utils.moveToTrash(testFile);
      log(`moveToTrash returned: ${result}`);
      expect(result).toBe(true);

      // Verify file no longer exists at original path
      let originalStillExists = true;
      try {
        await access(testFile);
      } catch {
        originalStillExists = false;
      }
      expect(originalStillExists).toBe(false);
      log("File successfully moved to trash");

      // Cleanup temp directory (it should be empty now)
      try {
        const { rmdir } = await import("fs/promises");
        await rmdir(tempDir);
      } catch {
        // Ignore cleanup errors
      }
    },
  }),

  defineTest({
    name: "quit function exists",
    category: "Utils",
    description: "Test that quit function is available",
    async run({ log }) {
      expect(typeof Utils.quit).toBe("function");
      log("quit function exists (not calling it!)");
    },
  }),

  // ---- Paths API tests ----

  defineTest({
    name: "paths object exists",
    category: "Utils",
    description: "Test that Utils.paths is available",
    async run({ log }) {
      expect(typeof Utils.paths).toBe("object");
      expect(Utils.paths !== null).toBe(true);
      log("Utils.paths is an object");
    },
  }),

  defineTest({
    name: "paths.home matches os.homedir()",
    category: "Utils",
    description: "Test that paths.home returns the correct home directory",
    async run({ log }) {
      const result = Utils.paths.home;
      expect(typeof result).toBe("string");
      expect(result).toBe(homedir());
      log(`paths.home = "${result}"`);
    },
  }),

  defineTest({
    name: "paths.temp matches os.tmpdir()",
    category: "Utils",
    description: "Test that paths.temp returns the correct temp directory",
    async run({ log }) {
      const result = Utils.paths.temp;
      expect(typeof result).toBe("string");
      expect(result).toBe(tmpdir());
      log(`paths.temp = "${result}"`);
    },
  }),

  defineTest({
    name: "paths OS directories return non-empty strings",
    category: "Utils",
    description: "Test that all OS-level path getters return non-empty strings",
    async run({ log }) {
      const osKeys = [
        "appData",
        "config",
        "cache",
        "logs",
        "documents",
        "downloads",
        "desktop",
        "pictures",
        "music",
        "videos",
      ] as const;

      for (const key of osKeys) {
        const value = Utils.paths[key];
        expect(typeof value).toBe("string");
        expect(value.length > 0).toBe(true);
        log(`paths.${key} = "${value}"`);
      }
    },
  }),

  defineTest({
    name: "paths app-scoped directories return non-empty strings",
    category: "Utils",
    description:
      "Test that userData, userCache, userLogs return paths based on version.json",
    async run({ log }) {
      const appKeys = ["userData", "userCache", "userLogs"] as const;

      for (const key of appKeys) {
        const value = Utils.paths[key];
        expect(typeof value).toBe("string");
        expect(value.length > 0).toBe(true);
        log(`paths.${key} = "${value}"`);
      }

      // App-scoped paths should contain an identifier-like segment
      // (e.g. "some.app.identifier") and not just be the base OS dir
      expect(Utils.paths.userData.length > Utils.paths.appData.length).toBe(
        true,
      );
      expect(Utils.paths.userCache.length > Utils.paths.cache.length).toBe(
        true,
      );
      expect(Utils.paths.userLogs.length > Utils.paths.logs.length).toBe(true);
      log("App-scoped paths are longer than their base OS dirs");
    },
  }),

  defineTest({
    name: "paths getters are stable across calls",
    category: "Utils",
    description: "Test that repeated access returns the same value",
    async run({ log }) {
      expect(Utils.paths.home).toBe(Utils.paths.home);
      expect(Utils.paths.downloads).toBe(Utils.paths.downloads);
      expect(Utils.paths.userData).toBe(Utils.paths.userData);
      log("All tested paths return stable values");
    },
  }),

  defineTest({
    name: "getAccentColor returns a hex color or null",
    category: "Utils",
    description: "Test Utils.getAccentColor's shape per platform and that it's stable across calls",
    async run({ log }) {
      const color = Utils.getAccentColor();
      log(`accent color on ${process.platform}: ${color}`);
      if (process.platform === "darwin") {
        expect(/^#[0-9a-f]{6}$/.test(color ?? ""), "macOS always has one").toBe(true);
      } else if (process.platform === "linux") {
        // Only desktops whose Settings portal publishes accent-color have one.
        expect(color === null || /^#[0-9a-f]{6}$/.test(color), "hex or null").toBe(true);
      } else {
        expect(color).toBe(null);
      }
      expect(Utils.getAccentColor()).toBe(color);
    },
  }),

  defineTest({
    name: "accent-color-changed follows the system accent",
    category: "Utils",
    description:
      "Changes the macOS accent color to red and back, and checks each change arrives as accent-color-changed with the color getAccentColor reads. Changes a system setting, so it only runs on CI",
    timeout: 20000,
    requires: { platform: "darwin" },
    async run({ log }) {
      if (!process.env.GITHUB_ACTIONS) {
        log("Not on CI: left the system accent color alone");
        return;
      }
      // What System Settings does: write the preference, then tell apps.
      const setAccent = (value: number | null) => {
        spawnSync("defaults", value === null ? ["delete", "-g", "AppleAccentColor"] : ["write", "-g", "AppleAccentColor", "-int", String(value)]);
        for (const name of ["AppleColorPreferencesChangedNotification", "AppleAquaColorVariantChanged"]) {
          const posted = spawnSync("osascript", [
            "-l",
            "JavaScript",
            "-e",
            `ObjC.import('Foundation'); $.NSDistributedNotificationCenter.defaultCenter.postNotificationNameObjectUserInfoDeliverImmediately('${name}', $(), $(), true)`,
          ]);
          log(`posted ${name}: exit ${posted.status} ${posted.stderr?.toString().trim() ?? ""}`);
        }
      };
      const nextChange = (timeoutMs = 5000) =>
        new Promise<string | null>((resolve, reject) => {
          const timer = setTimeout(() => {
            Electrobun.events.off("accent-color-changed", handler);
            reject(new Error(`no accent-color-changed within ${timeoutMs} ms`));
          }, timeoutMs);
          const handler = (event: { data: { color: string | null } }) => {
            clearTimeout(timer);
            Electrobun.events.off("accent-color-changed", handler);
            resolve(event.data.color);
          };
          Electrobun.events.on("accent-color-changed", handler);
        });

      const before = Utils.getAccentColor();
      const original = spawnSync("defaults", ["read", "-g", "AppleAccentColor"]).stdout?.toString().trim();
      log(`before: ${before} (AppleAccentColor ${original || "unset"})`);
      try {
        const toRed = nextChange();
        setAccent(0);
        await sleep(1000);
        log(`getAccentColor after the change: ${Utils.getAccentColor()}`);
        const red = await toRed;
        log(`changed to: ${red}`);
        expect(red !== null && red !== before, "a new color").toBe(true);
        expect(Utils.getAccentColor()).toBe(red);
      } finally {
        const back = nextChange().catch(() => null);
        setAccent(original ? Number(original) : null);
        log(`restored: ${await back}`);
      }
      await sleep(200);
      expect(Utils.getAccentColor()).toBe(before);
    },
  }),
];
