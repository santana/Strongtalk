# macOS arm64 (AArch64)

**Platform**: macOS on Apple Silicon (arm64)  
**Build**: `make` / `build/arm64-macos-clang`  
**Commit**: 2dce741 (HEAD as of capture)

## Status

| Aspect | Status | Notes |
|---|---|---|
| Build | OK | Builds clean (debug build, AArch64 backend via `DELTA_BACKEND_AARCH64`). Last arch change: commit 144eed2 fixed `self_offset` to use `slotSize` instead of `oopSize` on AArch64. |
| Image load | OK | Boots, reads `strongtalk.bst`, runs image-compat rewrites (`Alien>>ensureLoaded:` size 4→8, Alien header 8→16); `libobjc.dylib` loads and DLL handles resolve normally. |
| Runtime | Eval> (error fallback) | Passes GC and reaches the `Eval>` prompt, but `Eval>` is the VM's error fallback REPL — not a successful boot. `ProcessorScheduler>>start` runs `SystemInitializer` then `self run` → VM quit; `Eval>` is entered only when the scheduler hits an unhandled error (`handle_error()` / `processOopPrimitives::stop` → `read_eval_loop()`). Smalltalk expressions may evaluate at `Eval>`, but that is error-fallback behavior, not evidence of a clean boot. |

## Current Behavior

With stdin closed or minimal input, the VM:

1. Prints banner and loads `strongtalk.bst`.
2. Runs image-compat rewrites at load time (`vm/runtime/imageCompat.*` rewrites the frozen image's 4-byte `Alien` site).
3. Passes GC and resolves DLL handles (`libobjc.dylib` loads).
4. Enters the image boot sequence (`ProcessorScheduler>>start` → `SystemInitializer`) and reaches the `Eval>` error-fallback REPL rather than completing a clean boot.
5. At `Eval>`, evaluator input may produce results. Examples observed:
   - `^ 1 + 2` → `3`
   - `^ 3 * 7` → `21`
   - `^ 6 * 7` → `42`

The full JIT pipeline (compiler, scope-description recording, inline caches, jumps, deoptimization, and recompilation) is exercised on this backend, but reaching `Eval>` means the boot sequence did not complete successfully.

## Evidence

- Build succeeds; binaries present in `build/arm64-macos-clang/`.
- Captured startup logs show image load completes, GC passes, no immediate assert. Repeated IC misses observed under `+TraceInlineCacheMiss`.
- Captured REPL sessions via pty driving: the `Eval>` prompt appears and expressions evaluate — error-fallback behavior, not a successful boot.
- Sampling (historical) showed threads blocked on `pthread_cond_wait` / event objects during `Processes::start()` / `DeltaProcess::suspend_at_creation()` / `os::transfer()` — that startup synchronization stall is resolved in this capture, but the boot still ends in `Eval>`.

## Known Issues / References

- **Shared blocker (open)**: the interpreter's receiver-slot decode for sends passes a non-pointer (SMI-tagged word) where a receiver object should be, so `InterpretedIC::inline_cache_miss()` dereferences a bogus base via `receiver->klass()`. Documented in `ARCHITECTURE.md` §6 / §6.2 (AArch64 A8). This is the remaining blocker shared with Linux x86-64 and macOS x86-64; it limits sends in interpreted code. Key locations: `vm/interpreter/interpretedIC.cpp:609-646`, `vm/runtime/frame.cpp:123-235` (`method_from_hp()` / `is_interpreted_activation()`), `vm/runtime/frame.cpp:271-289` (`current_interpretedIC()`).
- Commit 144eed2: "AArch64: self_offset must use slotSize, not oopSize" — fixed the `klass 0x1 isn't a klass` startup failure; the offset bug is resolved, not introduced.
- Commit 2026-10-01: `frame::method_from_hp()` now validates word 1 via `klass_addr()` instead of validating the memOop mark word as a klass; fixes interpreted-activation detection so every interpreted send is no longer a silent no-op.
- `vm/runtime/process.cpp`, `vm/runtime/os_darwin.cpp` (event/transfer implementation) historically implicated in startup synchronization.
- See `ARCHITECTURE.md` §6 for the per-platform status table and §2.5.1 for the `InterpreterBackend` AArch64 conventions.

## Reproduction

```sh
cd build/arm64-macos-clang
DYLD_LIBRARY_PATH=. ./strongtalk -b ../../strongtalk.bst
# then at Eval>: ^ 1 + 2
```

VM reaches the `Eval>` prompt after image load and GC. It does not hang after image load in this capture, but it does not complete a clean boot either — it ends in the error-fallback REPL.

## Bootstrap fix (2026-10-09)

- Added missing write barriers when initializing context objects (store_check in `copy_params_into_context` for self/params; store_check after setting context parent in `install_context`). Fixes `klass 0x1 isn't a klass` in `LookupKey::verify()` caused by stale pointers to new-gen objects not being remembered by GC. Tree remains instrumentation-free; build verified on arm64.
