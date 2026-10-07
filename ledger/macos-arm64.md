# macOS arm64 (AArch64)

**Platform**: macOS on Apple Silicon (arm64)  
**Build**: `make` / `build/arm64-macos-clang`  
**Commit**: 144eed2 (HEAD as of capture)

## Status

| Aspect | Status | Notes |
|---|---|---|
| Build | OK | Builds clean (debug build). Last change: commit 144eed2 fixed `self_offset` to use `slotSize` instead of `oopSize` on AArch64, resolving the previous "klass 0x1 isn't a klass" startup failure. |
| Image load | OK | Boots, reads `strongtalk.bst`, runs image-compat rewrites (`Alien>>ensureLoaded:` size 4→8, Alien header 8→16). |
| Runtime | HANG | After image load, VM enters scheduler/Delta boot path and hangs. It does not reach `Eval>` or crash with an obvious assert in normal operation. |

## Current Failure

When run interactively, the VM prints the banner and image-load messages, then suspends/hangs in the startup transfer path. Backtraces sampled from the running process (thread states) show:

- Main thread waits in `DeltaProcess::suspend_at_creation()` -> `os::wait_for_event()` (`__psynch_cvwait`)
- VMProcess thread is in `VMProcess::transfer_to()` -> `os::transfer()` waiting on events
- The Delta thread is running in the interpreter/JIT startup path performing sends (e.g. megamorphic sends) as part of `DeltaProcess::launch_delta()` calling into Smalltalk

Sample trace shows heavy activity in the Delta startup thread with inline cache misses (e.g. repeated `InterpretedIC lookup (1-ProcessorScheduler class, #initialize)`) while synchronization waits are happening in the transfer/event layer. The hang is consistent with the commit message description: the earlier failure was masked; after 144eed2 the path proceeds further to the event/transfer coordination and exposes a pre-existing startup deadlock in `os::transfer` on macOS (AArch64).

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
