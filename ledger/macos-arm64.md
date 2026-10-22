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

### Defensive hardening (2026-10-13)
- `01bf5d7` validates the receiver in `InterpretedIC::inline_cache_miss()` before the `klass()` read: a SMI resolves to `Universe::smiKlassObj()`, and `klass()` is read only for `is_mem()`/`is_block()`/`is_proxy()` objects, so a corrupted receiver slot no longer dereferences a bogus base.
- `8506a1d` makes `frame::hp()`/`set_hp()` reject structurally-invalid frames (null or inverted fp/sp, an hp slot outside the frame, or an implausible hp value) instead of dereferencing them.
- `984c55b` adds a temporary frame-walk `lprintf` diagnostic in `DeltaProcess::frame_iterate()` (only its message claims "defensive checks"); it is not a bounds-check change.
- These guard the shared send/GC path against corrupted frames; they do not fix the receiver-slot decode, and the arm64 runtime capture above (commit 389d3d9) is otherwise unchanged.

### Primitive-call ABI fix and the remaining `...ifFail:` failure-block blocker (2026-10-14)

The status table above is stale: the VM no longer hangs after image load. It boots
the frozen image, passes GC, reaches `Eval>`, and evaluates expressions. `stest`
boots and starts a test run, but the test process dies during
`initializeSmalltalkEnvironment()` → `Smalltalk initialize` → `Character
initialize` → `initializeClassifications`.

**Landed fixes**
- *Interpreter primitive-call operand consumption (AArch64).* The primitive glue
  popped exactly one 16-byte slot per call, but the contract is to consume the
  primitive's parameter slots (`primitive_desc::number_of_parameters()`, `_flags`
  bits 0–7). The arity is now resolved at run time — `primitives::number_of_argument_slots(fntype)`
  (direct-mapped cache in `vm/prims/prim.cpp`) is called from
  `InterpreterBackend::passPrimitiveCallArgs` with the bytecode pointer parked in
  x24 and the slot count carried in x25, and `popPrimitiveCallArguments` pops that
  many slots. `max(1, nparams)` is intentional: the glue always `pushl`s one slot.
  On the reference AArch64 build this made the previously wrong `=`,
  `==`/predicted-Smi equality and `betweenAnd:` results correct; e.g.
  `probeDE(eqs=1): eax=0x0 edx=0x0`, `equalsStr=1`, `eqsSame=1`, `btwn=1`.
- *AArch64 stub immediates.* `addl(x11, 8)` → `addl(x11, slotSize)` and
  `sub(x17, 1)` → `decl(x17)` in `vm/code/stubRoutines.cpp` (a `slotSize` is 16
  bytes, not 8).
- *Image-compat: LP64 `struct stat`.* `patch_statbuffer_offsets()` in
  `vm/runtime/imageCompat.cpp` rewrites the frozen image's `StatBuffer` field
  offsets to the LP64 macOS layout, wired from `load_image()`.
- *`proxy_prims.cpp`.* `int` → `intptr_t` for the offset arithmetic.
- *Test harness.* The mock scheduler (`test/runtime/testProcess.cpp`) must never
  resume a process that has already stopped, or the image's `Error>>defaultAction`
  re-raises forever and the run ends in a stack-overflow (SIGILL). Failures are
  reported and surfaced through `test_run_failed()`.

**Remaining blocker — the `...ifFail:` failure block runs after a successful primitive.**
- `strongtalk.bst` was built for 32-bit and where possible should be converted
  rather than regenerated (the tree cannot rebuild it), so verify against the
  frozen image.
- Reproduction: `printf "^ Character initializeClassifications\n" | stest -b strongtalk.bst`
  raises `ProcessExplicitError` whose message text is the `Classifications`
  `ByteArray` (128 bytes, element 49 = 1) — i.e. `Object>>error:` was invoked with
  the *receiver* rather than an error symbol.
- Mechanism: `classify:as:` is
  `chars do: [:c <Character> | Classifications at: c asciiValue + 1 put: ((Classifications at: c asciiValue + 1) bitOr: mask)]`.
  The relevant primitive is `primitiveIndexedByteAt:put:ifFail:` →
  `byteArrayPrimitives::atPut` (`vm/prims/byteArray_prims.cpp:138`). On **success**
  it stores the byte and `return receiver;` (line 161); on *failure* it can only
  return `markSymbol(vmSymbols::out_of_bounds())` etc. So the error message being
  the receiver means the inline `[:err | self error: err]` ran with `eax` = the
  success result, i.e. the compiled site fell through into the failure block
  instead of skipping it. Consistent with this, `classify:as:` writes element 49
  (the first index) before the error surfaces — the write succeeds and the error
  follows immediately.
- The failure-path glue itself is shared and looks right:
  `call_primitive_can_fail` (`vm/interpreter/interpreter.cpp:1916`) does
  `testb(eax, Mark_Tag_Bit); jcc(notZero, failed); movl(ecx, [esi-oopSize]);
  addl(esi, ecx); ...` and binds `failed` to `andl(eax, ~Mark_Tag_Bit)`, matching
  `_dr_from_primitive_call_with_failure_block` (line 2148). `advance_aligned`
  (line 744) and the entry/offset slot layout are consistent with the x86
  reference. The suspect is therefore either the value of the stored jump offset
  (`[esi-oopSize]`, written by the image compiler) or its width under the 64-bit
  slot layout — both of which must be checked against image-compiler output.
- `=`, `<`, `+` etc. still behave correctly because their call sites are
  optimistically inlined (no failure block), so this path is otherwise unexercised
  at startup.

**Harness exit code is masked (not yet fixed).** On an unhandled error the
image's `ProcessorScheduler>>stopWithError:` → the `Process stop` primitive takes
its *scheduler* branch (`vm/prims/process_prims.cpp:188`), which calls
`evaluator::read_eval_loop()`. With stdin exhausted, the REPL calls
`os::fatalExit(0)` (`vm/runtime/evaluator.cpp:531`) → `_exit(0)`, so the process
always exits `0` and `main`'s `return test_run_failed() ? 1 : 0`
(`test/main/main.cpp`) never takes effect. `make test` therefore "passes"
vacuously while the test process is actually dying; the real fix is to make the
harness-presence path exit non-zero (or prevent the scheduler branch from entering
the REPL under a test run).
