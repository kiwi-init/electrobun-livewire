# Electrobun: Livewire — rules for agents and people

Livewire is Kiwi Init's fork of [blackboardsh/electrobun](https://github.com/blackboardsh/electrobun).
These rules win over `CLAUDE.md`, which is upstream's.

## Remotes

- `origin` is `kiwi-init/electrobun-livewire`. Everything you push goes here.
- `upstream` is `blackboardsh/electrobun`, fetch only:

  ```sh
  git remote add upstream https://github.com/blackboardsh/electrobun.git
  git remote set-url --push upstream DISABLED-push-to-origin-instead
  ```

## Branches

- `main` is always buildable and only changes through a pull request. Never force-push it.
- Work on `lw/<task>-<slug>`, e.g. `lw/T23-browserview-remove-rpc`. One fix per branch and per PR.

## Pull requests

- Open them against `kiwi-init/electrobun-livewire` `main`.
- The body names the Kiwi task (`T23`), what was broken, and how you verified it: the platform and the commands you ran.
- CI (`pr-checks.yml`) typechecks and builds every PR on macOS. It's advisory: nobody waits on it to review or merge. If it goes red after a merge, fix it in the next PR.

- Someone other than the author reviews (`kiwi task review T23`). The lead merges, with a squash, so each fix lands as one commit on `main`.

## Testing

- SDK logic (BrowserView, BrowserWindow, rpc) runs under `bun test` with no native libraries: install the fake from `package/src/sdks/main/__tests__/fakeFFI.ts` before importing the SDK and assert on the `ffi.request` calls it records. From `package/`: `node scripts/run-bun-test.js src/sdks/main`.
- Native behaviour needs Kitchen. Locally it runs in background mode by default (`ELECTROBUN_BACKGROUND=1`): windows never take the keyboard, cursor or focus, and the few tests that need the foreground are skipped. Never set `KITCHEN_FOREGROUND=1` on a machine a person is using. Still prefer CI for the full suite: the machine is shared and has 8 GB.

  ```sh
  gh workflow run kitchen-test.yml --ref <branch> -f test="<test name>|all" -f runner=macos-14
  gh workflow run kitchen-test.yml --ref <branch> -f test="<test name>" -f runner=ubuntu-24.04 -f display=wayland
  ```

  Interactive tests (a real drag, notification buttons) are for a person to run when they choose.

## Driving a running app

Start any Livewire app with `ELECTROBUN_AGENT_CONTROL=1` (or `runtime.agentControl: true` in its config) and agents can see and use it: list windows, read text, find controls, click, type, evaluate JS, tail the console, take screenshots (native WebKit views on macOS and Linux), run app-registered actions. It listens on a unix socket only your user can open (macOS and Linux).

```sh
claude mcp add livewire -- bun <repo>/package/src/sdks/main/agent/cli.ts mcp   # tools for Claude Code
bun package/src/sdks/main/agent/cli.ts eval "document.title"                    # or from a shell
```

Apps expose their own capabilities with `AgentControl.registerAction(name, { description, params, handler })` from `electrobun/bun`.

## Upstream

Livewire is a hard fork. We don't track or sync upstream, and we never open pull requests there.

- Take an upstream change only on purpose: cherry-pick it in its own PR and say where it came from.
- Never push upstream's tags to `origin` (`git push --tags`, `--follow-tags`, `--mirror`). They'd sit next to Livewire's own `v*` tags. `release.yml` (upstream's npm and Hutch catalog pipeline) no longer runs on tag pushes; never dispatch it.

## Releases

Livewire is versioned like Channels: CalVer `YYYY.M.PATCH` (2026.10.0, 2026.10.1, ...; a new month starts again at `.0`). `livewire-release.yml` publishes each one as the latest GitHub release, "Livewire YYYY.M.PATCH", tagged `vYYYY.M.PATCH`. Don't run `hutch push:*` or `release.yml`, and don't push `v*` tags by hand: the workflow creates the tag.

1. `node package/scripts/set-livewire-version.mjs --next` stamps the next version everywhere (or pass one, `... set-livewire-version.mjs 2026.11.0`); open a PR with it.
2. After it merges, run `gh workflow run livewire-release.yml --ref main`. It builds all six platforms, publishes the release with the devkit and its index, and then has Kitchen install it through Hutch. On any other ref it's a dry run that publishes nothing.

To build an app (like Channels) against a release:

```sh
export ELECTROBUN_RELEASES_BASE_URL=https://github.com/kiwi-init/electrobun-livewire/releases/download
# hutch.config.ts: electrobun: { version: "YYYY.M.PATCH" }
```
