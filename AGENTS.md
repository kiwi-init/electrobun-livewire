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

## Driving a running app

Start any Livewire app with `ELECTROBUN_AGENT_CONTROL=1` (or `runtime.agentControl: true` in its config) and agents can see and use it: list windows, read text, find controls, click, type, evaluate JS, tail the console, take screenshots (macOS today), run app-registered actions. It listens on a unix socket only your user can open (macOS and Linux).

```sh
claude mcp add livewire -- bun <repo>/package/src/sdks/main/agent/cli.ts mcp   # tools for Claude Code
bun package/src/sdks/main/agent/cli.ts eval "document.title"                    # or from a shell
```

Apps expose their own capabilities with `AgentControl.registerAction(name, { description, params, handler })` from `electrobun/bun`.

## Upstream

Livewire is a hard fork. We don't track or sync upstream, and we never open pull requests there.

- Take an upstream change only on purpose: cherry-pick it in its own PR and say where it came from.
- Never push upstream's tags to `origin` (`git push --tags`, `--follow-tags`, `--mirror`). `release.yml` runs on any `v*` tag push and would cut a release from this fork.

## Releases

Don't run `hutch push:*`, `release.yml` or any `v*` tag until Livewire owns its release pipeline. Those publish Electrobun versions under upstream's names.
