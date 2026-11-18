# JSC host conformance

"Works the same everywhere" is checked here, not by hand. Each `*.mjs` in this
folder runs twice:

- on the **reference**, the runtime apps run on today: Bun, or Node for a test
  that starts with `// oracle: node` (`socket.mjs`: Bun's `net.Socket({fd})`
  doesn't read pipes, which is why the SDK polls on Bun);
- **bundled, on this OS's JSC host** (`jsc-run`), as an app would be.

A test passes when both print the same lines and exit with the same code. A
test whose reference crashes is skipped, not failed: Bun 1.4.4 on Intel macOS
segfaults on a threadsafe `JSCallback` called from another thread
(`ffi.mjs`).

```sh
bun package/src/jsc-host/conformance/run.ts [out dir] [name filter]
```

| Test | Covers |
|---|---|
| `timers.mjs` | delays and their order, arguments, clear, intervals, immediates vs zero-delay timeouts, `Timeout` methods |
| `microtasks.mjs` | promise jobs, `queueMicrotask`, async/await, thenables, combinators, microtasks between timers and immediates |
| `fs.mjs` | sync, promise and error shapes (code, syscall, errno, message) for the fs calls the SDK and Channels use |
| `socket.mjs` | `net.Socket({fd})` over a FIFO: data, chunks, destroy, close |
| `crypto.mjs` | NIST GCM vectors, seeded AES-GCM up to 256 KB, tamper and tag-length rejection, randomness |
| `ffi.mjs` | `bun:ffi` calls and callbacks over libffi: every number type, more arguments than registers, threadsafe callbacks from another thread |
| `net.mjs` | `node:net` over unix sockets and TCP: listen/connect, line protocols, half-close and `allowHalfOpen`, 2 MB with backpressure, idle timeouts, ENOENT/ECONNREFUSED/EADDRINUSE, socket-file cleanup, `unref` |
| `bun-socket.mjs` | `Bun.listen`/`Bun.connect` (unix and TCP): handlers, partial writes and `drain`, `end`, `stop`, `connectError`, `timeout` |
| `fetch.mjs` | `fetch` against a local Bun server (`http-fixture.ts`): status, headers, bodies (string, bytes, params, Blob, streams), redirect modes, 3 MB and streamed responses, abort and timeout signals, refused connections; `Headers`, `Request`, `Response`, `Blob`, `AbortSignal`, `EventTarget`, `DOMException` |
| `websocket.mjs` | `WebSocket` against the same server: subprotocols, `readyState`, handlers and listeners, text, UTF-8 and binary frames per `binaryType`, a 200 KB message, closes from either side, close-code rules, refused connections; the same over `ws+unix://` (the fixture also serves a unix socket) |
| `spawn.mjs` | `child_process` (spawn, exec, execFile, sync forms: pipes, stdin, timeouts, kill, ENOENT, maxBuffer, detached), `Bun.spawn`/`spawnSync`/`which`, `process.kill`; a non-executable file on PATH (ENOENT, as Bun) |
| `zlib.mjs` | fixtures from Bun's zlib, round trips at each level and format, gzip members, error shapes, `crc32`, callbacks, streams through `pipeline`, `Bun.gzipSync` and co. |
| `readline.mjs` | line endings (LF, CRLF split across chunks, lone CR), split UTF-8, the async iterator, `question`, close |
| `secrets.mjs` | `Bun.secrets` get/set/delete on the real keychain (CI's macOS job only: `LIVEWIRE_KEYCHAIN_TEST`), argument errors everywhere |

Tests print only deterministic text (no random values, times or paths), so the
outputs can be compared line by line. A new OS's host passes when this suite
passes on it.
