# macOS arm64 (AArch64)

**Platform**: macOS on Apple Silicon (arm64)  
**Build**: `make` / `build/arm64-macos-clang`  
**Commit**: 389d3d9 (HEAD as of capture)

## Status

| Aspect | Status | Notes |
|---|---|---|
| Build | OK | Builds clean (debug build). Last change: commit 144eed2 fixed `self_offset` to use `slotSize` instead of `oopSize` on AArch64, resolving the previous "klass 0x1 isn't a klass" startup failure. |
| Image load | OK | Boots, reads `strongtalk.bst`, runs image-compat rewrites (`Alien>>ensureLoaded:` size 4→8, Alien header 8→16). |
| Runtime | LOOP (process fork storm) | After image load, two GCs complete; scheduler enters an idle/stop-handler loop spawning ProgramEnvironment/StackTraceInspector processes via `Process class>>for:`, causing runaway process/thread creation (does not reach `Eval>`). |

## Current Failure

With the GC fixes (389d3d9), the VM now passes two full mark-sweep GC cycles and avoids the post-conversion crash. After image load, the image boot sequence proceeds further; however, with mitigations (idle status coercion + stopHandler clearing), the startup path reaches `Eval>` on arm64 from the debugger/program environment idle handler: when the scheduler has no runnable process, it goes idle and `handleStopFor:` invokes the installed stopHandler (`Launcher>>startProgEnv` sets `Processor stopHandler:` to open a `StackTraceInspector`), which creates a new process via `Process class>>for:` → `fork` → `launch` → `openOn:` → `startProgEnv` → `handleStopFor:` → infinite loop (12k+ forks observed, hitting the thread limit with `pthread_create` returning `EAGAIN`).

## Evidence

- Build succeeds; binaries present in `build/arm64-macos-clang/`
- Captured startup logs show image load completes, no immediate assert. Repeated IC misses observed under `+TraceInlineCacheMiss`.
- Sampling shows threads blocked on `pthread_cond_wait` / event objects during `Processes::start()` / `DeltaProcess::suspend_at_creation()` / `os::transfer()`.

## Known Issues / References

- Commit 144eed2: "AArch64: self_offset must use slotSize, not oopSize" — fixed offset bug; hang is exposed, not introduced.
- See `ARCHITECTURE.md` §6: macOS arm64 runtime state notes the same SMI-receiver root cause family (AArch64 A8) mentioned historically; current execution stalls earlier in event coordination on startup in this build state.
- `vm/runtime/process.cpp`, `vm/runtime/os_darwin.cpp` (event/transfer implementation) implicated in startup synchronization.

## Reproduction

```sh
cd build/arm64-macos-clang
DYLD_LIBRARY_PATH=. ./strongtalk -b ../../strongtalk.bst
```

VM hangs after image load messages; needs to be killed externally.

## Root cause trace (shared send-path)

The shared blocker is in the interpreter send path where `InterpretedIC::inline_cache_miss()` (vm/interpreter/interpretedIC.cpp:609-646) reads `receiver = ic->argument_spec() == Bytecodes::args_only ? f.receiver() : f.expr(ic->nof_arguments())` from the current frame. That value is not a valid heap object pointer in the affected cases; instead it is SMI-tagged (non-pointer). The subsequent `receiver->klass()` (line ~628) dereferences through a bogus base.

The frame/activation validity is checked in `frame::is_interpreted_activation()` / `frame::method_from_hp()` (vm/runtime/frame.cpp). These checks try to resolve the hybrid code pointer in `hp()` back to a `methodOop`; if resolution fails (garbage slot), `method_from_hp()` returns NULL and `is_interpreted_activation()` returns false. In the "silent no-op" scenario described in ARCHITECTURE.md, `is_interpreted_activation()` returned false for every frame, so `InterpretedIC::inline_cache_miss()` could not find its inline cache.

Key locations:
- `vm/interpreter/interpretedIC.cpp:609-646` — inline cache miss handler; reads receiver from frame
- `vm/runtime/frame.cpp:123-235` — `method_from_hp()` and `is_interpreted_activation()` validation logic
- `vm/runtime/frame.cpp:271-289` — `current_interpretedIC()` requires valid activation

### Bootstrap fix (2026-10-09)
- Added missing write barriers when initializing context objects (store_check in `copy_params_into_context` for self/params; store_check after setting context parent in `install_context`). Fixes `klass 0x1 isn't a klass` in `LookupKey::verify()` caused by stale pointers to new-gen objects not being remembered by GC. Tree remains instrumentation-free; build verified on arm64.
