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
| `ffi.mjs` | `bun:ffi` calls and callbacks, including threadsafe ones from another thread |
| `net.mjs` | `node:net` over unix sockets and TCP: listen/connect, line protocols, half-close and `allowHalfOpen`, 2 MB with backpressure, idle timeouts, ENOENT/ECONNREFUSED/EADDRINUSE, socket-file cleanup, `unref` |
| `bun-socket.mjs` | `Bun.listen`/`Bun.connect` (unix and TCP): handlers, partial writes and `drain`, `end`, `stop`, `connectError`, `timeout` |
| `spawn.mjs` | `child_process` (spawn, exec, execFile, sync forms: pipes, stdin, timeouts, kill, ENOENT, maxBuffer, detached), `Bun.spawn`/`spawnSync`/`which`, `process.kill` |

Tests print only deterministic text (no random values, times or paths), so the
outputs can be compared line by line. A new OS's host passes when this suite
passes on it.
