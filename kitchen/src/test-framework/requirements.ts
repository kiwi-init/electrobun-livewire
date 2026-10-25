import type {
  TestDefinition,
  WindowRenderer,
} from "./types";

export function getTestSkipReason(
  test: TestDefinition,
  availableRenderers: readonly WindowRenderer[],
  platform: string = process.platform,
): string | undefined {
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
