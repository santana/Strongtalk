# Linux x86-64

**Platform**: Linux x86_64 (native/amd64 Docker build)  
**Build**: `build/x86_64-linux-gcc` (built in Docker container `strongtalk:linux-tools`)  
**Commit**: 144eed2

## Status

| Aspect | Status | Notes |
|---|---|---|
| Build | OK | Builds clean in Docker/amd64 environment. |
| Image load | OK | Reads `strongtalk.bst`, applies image-compat patches. |
| Runtime | DEGRADED / UNSTABLE | Boots and runs, but the system enters an error/scheduler loop and produces heavy diagnostic spew (register dumps) from signal handlers; eventually may segfault or be killed. Does not reach a stable `Eval>` prompt in the captured runs. |
| Behavior | Looping | After image load, the scheduler encounters errors and repeatedly re-raises; `os_dump_context()` is called frequently (e.g. in `os::suspend_thread()` on Linux), producing continuous register dumps to stdout. |

## Current Failure

Captured runs show:

- Banner, image load, compat messages appear.
- System enters "Unhandled error in the scheduler" / `ProcessExplicitError` path.
- Register state (`RAX/RBX/RCX/.../RIP/RSP/RBP/RDI/RSI`) is dumped repeatedly (thousands of lines in < 90 seconds).
- Stack trace eventually shows frames in the error-handling path (e.g. `Error>>defaultAction`, exception handling) similar to x86-64 macOS startup error machinery, but on Linux this is accompanied by frequent `os_dump_context()` calls causing massive output; process may exit with segfault (exit code 139 observed in some pty runs).
- Basic evaluation attempt (`^ 3 + 4`) produced register dumps interleaved with partial output (`7` appeared among dumps), indicating the evaluator can compute but signal/context dumping is overwhelming/unstable.

The README states: Linux x86-64 boots, loads image, runs into image's error handler then "Unhandled error in the scheduler re-raises in a loop (~300k register dumps in 45 s); never reaches Eval>".

Observed behavior matches: noisy, looping on scheduler errors with heavy diagnostic output.

## Evidence

- Docker build of `x86_64-linux-gcc` succeeds.
- `script`/pipe captures show continuous register dumps (from `vm/runtime/os_linux.cpp::os_dump_context2()` and calls to `os::suspend_thread()`/related paths). Many dump lines repeat the same register values.
- Exit codes vary (0 with timeout after many dumps, or 139 on segfault).

## Known Issues / References

- `vm/runtime/os_linux.cpp`: `os_dump_context()` prints full register context (see lines where RAX/RBX/etc. printed). Called from `os::suspend_thread()` (line ~584 in that file) — frequent context dumps explain the observed noise.
- README/ARCHITECTURE note Linux x86-64 never reaches `Eval>`; this matches the captured behavior.
- Signal handling path on Linux differs from macOS (different ucontext access); the debug output is very verbose.

## Reproduction

```sh
docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src/build/x86_64-linux-gcc strongtalk:linux-tools \
  bash -c "LD_LIBRARY_PATH=. timeout 60 script -q -c './strongtalk -b ../../strongtalk.bst' /tmp/out.log; tail -40 /tmp/out.log"
```

Produces large register dump volume with stack traces from scheduler error handling.
