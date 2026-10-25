// Local Kitchen runs never take the user's keyboard, mouse or screen: they
// turn on Electrobun's background mode (ELECTROBUN_BACKGROUND=1), so windows
// open without activating the app and full screen is off, and tests marked
// `requires: { foreground: true }` are skipped. CI (CI=true) runs everything
// in the foreground, as does KITCHEN_FOREGROUND=1 locally.
//
// Imported first by the Kitchen entry point, so it's set before any window.

export function useBackgroundMode(env: Record<string, string | undefined> = process.env): boolean {
  if (env["ELECTROBUN_BACKGROUND"] === undefined) {
    const foreground = env["CI"] === "true" || env["KITCHEN_FOREGROUND"] === "1";
    env["ELECTROBUN_BACKGROUND"] = foreground ? "0" : "1";
  }
  return env["ELECTROBUN_BACKGROUND"] === "1";
}

export function isBackgroundMode(env: Record<string, string | undefined> = process.env): boolean {
  return env["ELECTROBUN_BACKGROUND"] === "1";
}

useBackgroundMode();
