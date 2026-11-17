# jsc-sdk

The milestone 1 check for `mainProcess: "jsc"` (#141): a small app on
Livewire's **unmodified** SDK whose main process runs on macOS's system
JavaScriptCore instead of a bundled Bun.

It passes when typed RPC works both ways, window events reach the app, and
closing the last window quits (exit 0); anything else exits 1.

```sh
# from an extracted electrobun-core-darwin-arm64 release
package/test-apps/jsc-sdk/assemble.sh <core dir> <out dir> [--bun <bun binary>]
"<out dir>/JSC SDK.app/Contents/MacOS/launcher"; echo $?
```

`--bun` also builds the same app on Bun for comparison. Needs Bun and Xcode's
clang at build time only.

## Built by Hutch (T431)

Any app can move to the JSC host the same way: a normal Hutch build for Bun,
then the devkit's postBuild hook (`package/src/jsc-host/post-build.ts`, shipped
as `api/jsc-host/` with a prebuilt host) swaps Bun for the host. See
`electrobun.config.ts`; the two lines that matter are
`scripts.postBuild: ".hutch/devkit/api/jsc-host/post-build.ts"` and allow-jit
in `mac.entitlements`.

```sh
cd package/test-apps/jsc-sdk
HUTCH_ELECTROBUN_DEVKIT_ROOT=$PWD/../../dist hutch electrobun build
"build/dev-macos-arm64/JSC SDK-dev.app/Contents/MacOS/launcher"; echo $?
```

- `LIVEWIRE_SDK_FOCUS=1` also checks the focus event. This takes the
  foreground, so use it only on a machine nobody is using (CI).
- `LIVEWIRE_SDK_HOLD_MS=4000` keeps the window open before quitting, so
  memory can be measured.
