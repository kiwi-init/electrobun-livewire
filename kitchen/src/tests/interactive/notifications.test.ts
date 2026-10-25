// Interactive Notification Tests - actions, reply and the dock

import { defineTest, expect } from "../../test-framework/types";
import Electrobun, { Utils } from "electrobun/main";

type Response = { id: string; action: string; reply?: string };

function nextResponse(id: string, timeoutMs: number): Promise<Response> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      Electrobun.events.off("notification-response", handler);
      reject(new Error(`no response to ${id} within ${timeoutMs / 1000}s`));
    }, timeoutMs);
    const handler = (event: { data: Response }) => {
      if (event.data.id !== id) return;
      clearTimeout(timer);
      Electrobun.events.off("notification-response", handler);
      resolve(event.data);
    };
    Electrobun.events.on("notification-response", handler);
  });
}

export const notificationTests = [
  defineTest({
    name: "Notification actions and reply",
    category: "Notifications (Interactive)",
    description:
      "A notification's reply field and action button report back to the app, and the dock shows a badge and progress meanwhile",
    instructions: [
      "macOS only. Allow notifications for Kitchen if asked",
      "The dock icon shows badge 2 and a progress bar while the test waits",
      "On the first notification, hover it, open Options or click Reply, type some text and send it",
      "On the second notification, choose the Archive button",
    ],
    interactive: true,
    timeout: 300000,
    async run({ log }) {
      Utils.setDockBadge(2);
      Utils.setDockProgress(0.5);
      try {
        const replyId = Utils.showNotification({
          title: "Ana (Kitchen)",
          body: "Lunch? Reply to this notification",
          reply: { placeholder: "Type a reply", buttonTitle: "Send" },
        });
        const reply = await nextResponse(replyId, 120000);
        log(`Reply response: ${JSON.stringify(reply)}`);
        expect(reply.action).toBe("reply");
        expect(typeof reply.reply).toBe("string");

        Utils.setDockProgress(1);
        const actionId = Utils.showNotification({
          title: "Kitchen",
          body: "Choose Archive",
          actions: [
            { id: "archive", title: "Archive" },
            { id: "delete", title: "Delete", destructive: true },
          ],
        });
        const action = await nextResponse(actionId, 120000);
        log(`Action response: ${JSON.stringify(action)}`);
        expect(action.action).toBe("archive");
      } finally {
        Utils.setDockBadge(null);
        Utils.setDockProgress(null);
      }
    },
  }),
];
