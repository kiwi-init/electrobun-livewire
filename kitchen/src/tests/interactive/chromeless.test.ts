// Interactive Chromeless/Transparent Window Tests

import { defineTest } from "../../test-framework/types";
import { BrowserView, BrowserWindow } from "electrobun/main";

export const chromelessTests = [
  defineTest({
    name: "Custom titlebar with window controls",
    category: "Chromeless Windows (Interactive)",
    description: "Test custom titlebar with draggable region and custom close/minimize/maximize buttons",
    instructions: [
      "A window with a custom titlebar will open",
      "Test the following:",
      "- Drag the window by the dark titlebar area",
      "- Click the colored buttons (close, minimize, maximize)",
      "- Verify text input works in the content area",
      "Close the window when done to pass the test",
    ],
    interactive: true,
    timeout: 120000,
    async run({ log }) {
      log("Opening custom titlebar test window");

      await new Promise<void>((resolve, _reject) => {
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        let winRef: BrowserWindow<any> | null = null;
        let isMaximized = false;

        const rpc = BrowserView.defineRPC<any>({
          maxRequestTime: 120000,
          handlers: {
            requests: {
              closeWindow: () => {
                log("Close button clicked");
                winRef?.close();
                return { success: true };
              },
              minimizeWindow: () => {
                log("Minimize button clicked");
                winRef?.minimize();
                return { success: true };
              },
              maximizeWindow: () => {
                if (isMaximized) {
                  log("Unmaximize button clicked");
                  winRef?.unmaximize();
                  isMaximized = false;
                } else {
                  log("Maximize button clicked");
                  winRef?.maximize();
                  isMaximized = true;
                }
                return { success: true };
              },
            },
            messages: {},
          },
        });

        winRef = new BrowserWindow({
          title: "Custom Titlebar",
          url: "views://playgrounds/custom-titlebar/index.html",
          renderer: "cef",
          frame: { width: 500, height: 700, x: 150, y: 50 },
          // 'hidden' titleBarStyle hides both titlebar AND native window controls
          titleBarStyle: "hidden",
          rpc,
        });

        winRef.setAlwaysOnTop(true);
        const win = winRef;

        win.on("close", () => {
          log("Window closed");
          resolve();
        });
      });

      log("Custom titlebar test completed");
    },
  }),

  defineTest({
    name: "First click on a background window's drag region moves it",
    category: "Chromeless Windows (Interactive)",
    description:
      "A background window takes its first click only on a drag region (macOS system WebKit)",
    instructions: [
      "A window with a custom titlebar will open",
      "Click into another app, so this window is in the background",
      "Press on the dark titlebar and drag in one motion: the window moves on the first click",
      "Put the window in the background again and click a colored button once: it only brings the window forward",
      "Close the window when done to pass the test",
    ],
    requires: { platform: "darwin", renderer: "native" },
    interactive: true,
    timeout: 120000,
    async run({ log }) {
      await new Promise<void>((resolve) => {
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        let winRef: BrowserWindow<any> | null = null;
        const rpc = BrowserView.defineRPC<any>({
          maxRequestTime: 120000,
          handlers: {
            requests: {
              closeWindow: () => {
                log("Close button clicked");
                winRef?.close();
                return { success: true };
              },
              minimizeWindow: () => {
                log("Minimize button clicked");
                return { success: true };
              },
              maximizeWindow: () => {
                log("Maximize button clicked");
                return { success: true };
              },
            },
            messages: {},
          },
        });

        winRef = new BrowserWindow({
          title: "First Click",
          url: "views://playgrounds/custom-titlebar/index.html",
          renderer: "native",
          frame: { width: 500, height: 700, x: 150, y: 50 },
          titleBarStyle: "hidden",
          rpc,
        });
        winRef.on("close", () => resolve());
      });
    },
  }),

  defineTest({
    name: "Transparent/borderless window for floating UI",
    category: "Chromeless Windows (Interactive)",
    description: "Test transparent window with custom-shaped floating UI elements",
    instructions: [
      "A transparent borderless window will open",
      "The window background should be transparent/see-through",
      "Test the following:",
      "- Verify you can see through the window background",
      "- Drag any of the floating cards to move the window",
      "- Drag empty transparent space covered by the stylesheet-defined overlay",
      "- Click the red close button when done",
      "Close the window when done to pass the test",
    ],
    interactive: true,
    timeout: 120000,
    async run({ log }) {
      log("Opening transparent window test");

      await new Promise<void>((resolve, _reject) => {
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        let winRef: BrowserWindow<any> | null = null;

        const rpc = BrowserView.defineRPC<any>({
          maxRequestTime: 120000,
          handlers: {
            requests: {
              closeWindow: () => {
                log("Close button clicked");
                winRef?.close();
                return { success: true };
              },
            },
            messages: {},
          },
        });

        winRef = new BrowserWindow({
          title: "Transparent Window",
          url: "views://playgrounds/transparent-window/index.html",
          renderer: "cef",
          frame: { width: 450, height: 500, x: 200, y: 100 },
          // 'hidden' titleBarStyle hides titlebar and native controls
          titleBarStyle: "hidden",
          // transparent: true makes window background see-through
          transparent: true,
          rpc,
        });

        winRef.setAlwaysOnTop(true);
        const win = winRef;

        win.on("close", () => {
          log("Window closed");
          resolve();
        });
      });

      log("Transparent window test completed");
    },
  }),
];
