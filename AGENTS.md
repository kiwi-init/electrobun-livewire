# Electrobun: Livewire — rules for agents and people

Livewire is Kiwi Init's fork of [blackboardsh/electrobun](https://github.com/blackboardsh/electrobun).
These rules win over `CONTRIBUTING.md` and `CLAUDE.md`, which are upstream's.

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

- Open them against `kiwi-init/electrobun-livewire` `main`. Never open one upstream unless the owner said so for that change.
- The body names the Kiwi task (`T23`), what was broken, and how you verified it: the platform and the commands you ran.
- No CI runs on pull requests yet. Until it does, run these from `package/` before asking for review, and list the platforms you built on:

  ```sh
  hutch typecheck
  hutch build:dev
  ```

- Someone other than the author reviews (`kiwi task review T23`). The lead merges, with a squash, so each fix lands as one commit on `main`.

## Upstream sync

- Sync to upstream release tags (`vX.Y.Z`, `vX.Y.Z-beta.N`), never to upstream `main`.
- Sync through a PR from `lw/sync-<tag>`, never by pushing to `main`.
- Never push upstream's tags to `origin` (`git push --tags`, `--follow-tags`, `--mirror`). `release.yml` runs on any `v*` tag push and would cut a release from this fork.
- Our patch list is everything on `main` since the last synced tag: `git log --first-parent <tag>..main`.

## Keep the diff with upstream small

Every line we change in an upstream file can conflict at the next sync.

- Put Livewire features (AppEngine first) in new modules behind small hooks in core.
- Don't reformat, rename or move upstream code you aren't fixing.
- Leave upstream's docs, changelog and `CONTRIBUTING.md` alone. Livewire docs go in new files.

## Releases

Don't run `hutch push:*`, `release.yml` or any `v*` tag until Livewire owns its release pipeline. Those publish Electrobun versions under upstream's names.
