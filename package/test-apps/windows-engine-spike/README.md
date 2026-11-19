# Windows engine spike (T241)

For #226: Windows has no standalone system JavaScript engine, unlike macOS
(JavaScriptCore.framework) and Linux (WebKitGTK's libjavascriptcoregtk). This
spike runs the same main process on the two candidates and on Bun, and
measures them:

- **A: a hidden WebView2.** Edge's V8, preinstalled on Windows 11. Native
  calls go through a synchronous host object (`wv2-host.cpp`), and messages go
  through `postMessage`.
- **B: QuickJS-ng linked into the host.** About 1 MB, interpreter only, no JIT
  (`qjs-host.c`).
- **Bun, today's runtime.** Native calls go through `bun:ffi` (`echo.dll`).

`workload.js` runs unchanged on all three. Each `*-main.js` only adapts the
`host` object: `echo` (a native call), `now` and `print`. `measure.ps1`
records, for each engine:
- launch time (process start to the first line of JS), 5 runs;
- idle memory of the whole process tree;
- the size shipped in the app;
- the workload's timings, 3 runs.

The CI job (`windows-engine-spike.yml`) builds everything on windows-2025 and
writes the table to the job summary.
