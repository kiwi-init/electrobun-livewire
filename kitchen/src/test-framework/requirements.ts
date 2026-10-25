import type {
  TestDefinition,
  WindowRenderer,
} from "./types";
import { isBackgroundMode } from "./background-mode";

export function getTestSkipReason(
  test: TestDefinition,
  availableRenderers: readonly WindowRenderer[],
  platform: string = process.platform,
  background: boolean = isBackgroundMode(),
): string | undefined {
  if (test.requires?.foreground && background) {
    return "needs the foreground (focus, activation or full screen); runs on CI, or locally with KITCHEN_FOREGROUND=1";
  }
  const platforms = test.requires?.platform === undefined ? [] : [test.requires.platform].flat();
  if (platforms.length > 0 && !platforms.some((candidate) => candidate === platform)) {
    return `requires ${platforms.join(" or ")}, running on ${platform}`;
  }
  const requiredRenderer = test.requires?.renderer;
  if (!requiredRenderer || availableRenderers.includes(requiredRenderer)) {
    return undefined;
  }

  return `requires the ${requiredRenderer.toUpperCase()} renderer, but this build only includes ${availableRenderers.join(", ") || "no renderers"}`;
}
