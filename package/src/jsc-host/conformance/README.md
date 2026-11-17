# JSC host conformance

"Works the same everywhere" is checked here, not by hand. Each `*.mjs` in this
folder runs twice:

- on the **reference**, the runtime apps run on today: Bun, or Node for a test
  that starts with `// oracle: node` (`socket.mjs`: Bun's `net.Socket({fd})`
  doesn't read pipes, which is why the SDK polls on Bun);
- **bundled, on this OS's JSC host** (`jsc-run`), as an app would be.

A test passes when both print the same lines and exit with the same code.

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

Tests print only deterministic text (no random values, times or paths), so the
outputs can be compared line by line. A new OS's host passes when this suite
passes on it.
