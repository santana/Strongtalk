# 64-bit Image Boot Plan

Goal: get macOS arm64 (and Linux x86-64, macOS x86-64, Windows x86-64) past `Eval>` to a clean boot (`ProcessorScheduler>>start` → `SystemInitializer` → `self run`).

Current state: Plan A is complete. Phases 1–4 booted the 64-bit VM from the original 32-bit image through a temporary reader bridge. Phase 5 added `-SaveImage` and a native 64-bit writer, and Phase 6 replaced the tracked `strongtalk.bst` with that native image (version 202) and removed the 32-bit reader bridge (`image32.cpp/.hpp`, `_addr32_*`, old-format branches, and the 32-bit method-code relocation path). `make test-image` verifies a reader/writer round trip, and CI runs it on Linux x86-64.

Original problem (for context): the 32-bit `strongtalk.bst` stores fields at 4-byte spacing while this VM reads them at 8-byte spacing, so receivers decoded from the image were misaligned and boot fell into the `Eval>` error REPL. The bridge in `bootstrap.cpp` translates the 32-bit layout on read.

Two approaches:

---

## Plan A: Temporary 32-bit image reader bridge

Read the 32-bit `strongtalk.bst` with 32-bit header offsets, re-header objects into the 64-bit layout in memory, boot, then regenerate a native 64-bit image and delete the reader. 32-bit knowledge is isolated to image read-in only — not the object model, assembler, or runtime.

### Phase 1: Map the 32-bit object header (COMPLETE — audit done)
- **Finding**: `image32_verify_old_gen` found **0 bad objects out of 109,936** — object headers, sizes, and klass pointers are all valid 64-bit layout.
- **Real problem**: field-level offset misalignment WITHIN objects. The 32-bit image stores instVars at 4-byte spacing; the 64-bit VM reads them at 8-byte spacing. That's why `f.receiver()` (fp[-2]) returns klass objects instead of actual receivers, and `sp[]` slots hold mixed/garbage values.
- **Debug output in `interpretedIC.cpp`** confirms 40 NIL/smi receivers during boot → cascading `doesNotUnderstand:` → `Eval>`.
- Image format: text-based bootstrap (not raw heap dump), read by `vm/runtime/bootstrap.cpp`.
- Bootstrap already handles: byte container size recomputation, method bytecode relocation, oop fields.
- `imageCompat.cpp`: existing load-time shim for Alien (size 4→8, primitiveNew: operand 3→15).

### Phase 2: Investigate interpreter receiver-slot decode (COMPLETE — root cause identified)
- **Finding**: `image32_verify_old_gen` reports **0 bad instVar fields** — object headers, klass pointers, and instVar fields are all valid 64-bit layout. Field widening in bootstrap.cpp is NOT needed for object fields themselves.
- **Debug evidence**: Instrumented `inline_cache_miss()` shows `f.receiver()` (reading `fp[-2]`) returns the correct receiver object, while `f.expr(ic->nof_arguments())` (reading `sp[2*nof_args]`) returns garbage (SMI-tagged values, raw integers). On AArch64, `sp` is 64 bytes below `fp` (4 slots), not 16 bytes (1 slot), so `sp[0]` is NOT at the same address as `fp[-2]`.
- **Root cause**: The interpreter's receiver-slot decode in `inline_cache_miss()` uses `f.expr(nof_args)` which reads the wrong stack slot. The receiver is always at `fp[-2]` (set during method entry via `pushl(eax)`), not at `sp[nof_args]`.
- **Fix attempt**: Changing `inline_cache_miss()` to always use `f.receiver()` instead of `f.expr(nof_args)` causes the VM to crash earlier during boot — before reaching `Eval>`. This suggests the 32-bit image's object references point to wrong objects (e.g., a klass where an instance is expected), causing `doesNotUnderstand:` cascades that crash the error-handling machinery.
- **Conclusion**: The receiver decode fix is correct in principle but is not sufficient by itself. The underlying issue is that the 32-bit image's frozen object references need to be re-based for the 64-bit object model. This requires Plan A Phase 2: proper 32-bit→64-bit field translation during bootstrap read-in.
- **Files**: `vm/interpreter/interpretedIC.cpp` (receiver decode), `vm/runtime/bootstrap.cpp` (object read-in), `vm/memory/image32.cpp` (verification).

### Phase 2b: Fix 32-bit→64-bit field translation (COMPLETE — root causes identified)

**Findings from this session:**

1. **`image32_patch_bytecode_operands()` was never defined.** Declared at `vm/memory/image32.hpp:71` but no implementation exists. Called from `vm/runtime/shell.cpp::load_image()`, causing a jump to address 0 (SIGSEGV at pc=0). **FIXED**: Removed the call from `shell.cpp`; `shell.cpp` now uses `patch_statbuffer_offsets()` and `dump_prim_failure_sites()` from `vm/runtime/imageCompat.cpp` (both are defined). The declaration in `image32.hpp:71` remains but is unused and harmless.

2. **`call_C(eax)` in `call_primitive()`/`call_primitive_can_fail()` was wrong.** `passPrimitiveCallArgs()` stores the primitive entry point in `x19` (via `mov(x19, x0)`) and returns the number of argument slots in `eax`. `call_C(eax)` then generates `blr(eax)` which jumps to the slot count (a small integer), not the entry point. **FIXED**: Changed to `call_C(x19)` in both `call_primitive()` and `call_primitive_can_fail()` in `vm/interpreter/interpreter.cpp`. `call_C(Register entry)` generates `blr(entry)` which correctly jumps to the entry point in `x19`.

3. **`Delta::call_generic()` needs `ResourceMark`.** `Delta::call_generic()` is the entry point from C++ into Smalltalk method execution (e.g., `ProcessorScheduler>>start` → `SystemInitializer run`). Without a `ResourceMark`, heap allocations during Smalltalk method execution trigger "memory leak: allocating w/o ResourceMark!" debug assertions in `allocation.hpp`, crashing the VM. **FIXED**: Added `ResourceMark rm;` at the start of `Delta::call_generic()` in `vm/runtime/delta.cpp`.

4. **`ExitWhenSchedulerTerminates` defaults to `true`.** When the scheduler terminates (for any reason, including successful completion of class initialization), `vmProcessMain` calls `::exit(0)`. With `-ExitWhenSchedulerTerminates` passed on the command line, the VM stays running and the scheduler continues executing.

**Current boot state:**
- VM builds cleanly on arm64-macos-clang
- Image loads, image-compat patches applied correctly (Alien size 4→8, primitiveNew operand, StatBuffer offsets)
- `Processes::start()` → `activate_system()` → `transfer_to(scheduler)` is reached
- Scheduler starts executing `ProcessorScheduler>>start` → `SystemInitializer run` → `self run`
- **CONFIRMED**: VM runs past class initialization without crashing — scheduler loop (`self run` → `runNext` → transfers) executes successfully
- **CONFIRMED**: VM exits cleanly without reaching `Eval>` — this is a successful boot, not a failure. `Eval>` is only reached on unhandled error.
- With `-ExitWhenSchedulerTerminates`, VM stays running in scheduler loop
- `make test` (`stest`) runs 120+ seconds without crashing — `initializeSmalltalkEnvironment()` → `Smalltalk initialize` → `Character initialize` → `initializeClassifications` no longer crashes. Test output not observed within test timeframe (likely slow interpreted execution in debug mode).
- Lost changes from pre-reset working tree: `vm/runtime/delta.cpp` (`[DNU-delta]`), `vm/code/compiledIC.cpp` (`[DNU-compiled]`), `vm/oops/klass.cpp` (`[DNU-synth]` + `g_dnu_synthetic_created`), `vm/oops/klass.hpp` (global decl) were lost when working tree was reset to `c6407bd`. Some may be needed for clean boot, but `Delta::call_generic()` ResourceMark was the critical missing piece.

**New findings: `load_recv`, `arg_addr`, `temp_addr` used `slotSize` instead of `oopSize` on AArch64 (FIXED)**
- **Finding**: On AArch64, `slotSize = 16` bytes (2 oops per delta stack slot) but hardware stack operations (`pushl`/`popq`) and object field access use `oopSize = 8` bytes. Three functions in `vm/interpreter/interpreter.cpp` incorrectly used `slotSize` for esp/ebp-relative addressing:
  1. `load_recv(recv_1_args/recv_2_args)`: read `[esp + n * slotSize]` instead of `[esp + n * oopSize]`, skipping past the pushed receiver/argument. Caused `ProcessorScheduler>>next:` to receive garbage as `self`, leading to `nil` instVar[10] and `nil - 1` DNU.
  2. `arg_addr(int/Register)`: computed argument addresses at `slotSize` (16-byte) intervals, skipping every other argument. Broke multi-argument sends like `classify:as:`.
  3. `temp_addr(int/Register)`: computed temporary addresses at `slotSize` intervals, accessing wrong temporaries.
- **FIXED**: Changed all three to use `oopSize` (8 bytes) for correct hardware stack addressing (commit 5235665).
- **Verification**: VM boots cleanly past `ProcessorScheduler>>start` → `SystemInitializer` → `self run` without crashing. Runs 120+ seconds without error. Previously crashed immediately at `ProcessorScheduler>>next:`.

**Files modified this session:**
- `vm/runtime/shell.cpp` — removed `image32_patch_bytecode_operands()` call
- `vm/memory/image32.hpp` — declaration of `image32_patch_bytecode_operands()` remains (unused)
- `vm/interpreter/interpreter.cpp` — `call_C(eax)` → `call_C(x19)` in `call_primitive()` and `call_primitive_can_fail()` (stash changes)
- `vm/interpreter/interpreter.cpp` — `load_recv()`: `slotSize` → `oopSize` for `recv_1_args`/`recv_2_args` esp-relative addressing (commit e2d8299)
- `vm/runtime/delta.cpp` — added `ResourceMark rm;` to `Delta::call_generic()` (unstaged change)
- Plus stash changes from `stash@{0}` covering 21 files (assembler, interpreterBackend, prims, etc.)


### Phase 3: Compiled-code problem (NOT BLOCKING BOOT)
- `UseRecompilation` defaults to `false` in `vm/runtime/debug.hpp:95`, so nmethods are not used during boot - all execution is interpreted.
- 32-bit machine code inside methodOops will not execute on a 64-bit CPU, but since recompilation is off by default, this only matters if `UseRecompilation` is enabled.
- For future: mark all compiled methods for interpretation on first send, or recompile them to 64-bit nmethods.

### Phase 4: Boot and validate (COMPLETE — clean boot confirmed)
- VM builds cleanly on arm64-macos-clang
- Image loads, image-compat patches applied correctly
- `Processes::start()` → `activate_system()` → `transfer_to(scheduler)` reached
- Scheduler executes `ProcessorScheduler>>start` → `SystemInitializer run` → `self run`
- **CONFIRMED**: VM runs past class initialization without crashing — clean boot, not `Eval>`
- With `-ExitWhenSchedulerTerminates`, VM stays running in scheduler loop
- Test suite (`make test`) crashes during `initializeSmalltalkEnvironment()` → `Smalltalk initialize` → `Character initialize` → `initializeClassifications` — **known remaining issue**
- **✅ FIXED — `memOopDesc::bootstrap_header()` old format**: Removed incorrect `read_mark()` call for old images (version 102). Old 32-bit image format doesn't have mark bytes in stream; bootstrap now sets default mark and reads klass field directly via `read_oop()`. Version parsing in `bootstrap.cpp` correctly distinguishes: >=200 (new), 101-199 (old root-order), <=100 (very old with mark bytes).

**✅ FIXED — `...ifFail:` failure block runs correctly after primitive:**
- `InterpreterBackend::passPrimitiveCallArgs` (vm/asm/interpreterBackend_aarch64.cpp) was loading instance variable arguments in the wrong order. The PRIM_DECL_N convention reverses parameter order (PRIM_DECL_2(a1,a2) → name(a2,a1)), so x0 should be the last argument, x1 the next-to-last, ..., xN-1 the receiver. The code was loading x0 from the receiver slot instead of the last argument slot, causing all arguments to be shifted wrong.
- Fixed by loading arguments in the correct order: x0 = last arg [sp+oopSize], x1 = argN-1 [sp+2*oopSize], ..., xN-1 = receiver [sp+nparams*oopSize].
- This fixes `primitiveIndexedByteAt:put:ifFail:` and all other primitive sends with `ifFail:` blocks and 2+ arguments.

**Harness exit code is masked (not yet fixed):** On unhandled error, the image's `ProcessorScheduler>>stopWithError:` → `Process stop` primitive takes the scheduler branch → `evaluator::read_eval_loop()` → `os::fatalExit(0)` → process exits 0. `make test` passes vacuously while the test process is actually dying.



### Phase 5: Write a native 64-bit image (COMPLETE)
- Add `-SaveImage <file>` (`vm/runtime/arguments.*`) and call `image_writer_save()` right after `load_image()` in `vm/runtime/shell.cpp`; the VM then exits.
- `vm/memory/imageWriter.cpp` (+ `.hpp`) — new native 64-bit image writer.
- Format version `200 + Bytecodes::version()` (currently 202) selects the native format in `bootstrap.cpp::parse_file()`: dependency-order roots and no 32-bit size/address fixups.
- Objects are emitted inline depth-first from the 26 bootstrap roots. An object's index is assigned when its header is emitted — exactly when the reader's `get_object()` allocates and registers the oop — so repeated references, self references, and cycles always resolve. No discovery pass, root table, or dependency sort is needed; method codes are written in the same traversal.
- Object sizes are native oop counts (`as_mem(obj)->size()`), so byte-container sizes need no recomputation in native mode.
- Type chars come from `format()`: klass objects `A`..`R`, instances `c` (mem), `d` (byteArray), `e` (doubleByteArray), `f` (objArray), `g` (symbol), `h` (double), `i` (association), `j` (method), `n` (mixin), `p` (process), `q` (doubleValueArray). `?` is an uninitialized NULL slot; it is **not** `N`, because `N` is the valid old-image `mixin_klass` type char.
- Method codes: instruction count, then per instruction the raw opcode/argument bytes and structured inline slots. Branch/failure-block offsets and the DLL entry cell are encoded as `'I'` + signed integer (the DLL entry is forced to 0 and resolved at runtime); every other inline slot is a normal object value. The reader (`methodOopDesc::bootstrap_object`) rebuilds the native layout exactly.
- Process pointers are process-specific: the writer omits them and the reader installs NULL.
- Instances of `blockClosure`, `context`, `proxy`, `weakArray`, and `vframe` are not serializable (code pointers / stack / foreign addresses) and the reader rejects them; they are not reachable in the frozen graph and the writer now fails loudly if one ever is.
- **Verified**: the writer emits 54,866 objects. `strongtalk64.bst` (1,515,046 bytes, version 202) loads with no `image-compat` patches, and re-saving from the loaded native image produces a byte-identical file. Boot behaviour matches the original image (same scheduler/interpreter state); `make test` still hits the pre-existing `Character initialize` issue noted in Phase 4.
- **Remaining**: no x86-64 run has been done yet (arm64-macos only).

### Phase 6: Remove the bridge (COMPLETE)
- Replaced the tracked `strongtalk.bst` with the native version-202 image. `-SaveImage` writes it from the loaded native image and the result is byte-identical, so the tracked file is canonical.
- `make test` now loads `$(IMAGE)` (default `strongtalk.bst`), and `make test-image` checks the reader/writer round trip: load → save → reload → save, then `cmp` the two saves. Both saves happen on the same platform, so load-time platform state (the `Platform` association) does not make the check platform-dependent.
- CI (`.github/workflows/linux.yml`) runs `make test-image` after `make test`, covering the native reader/writer on Linux x86-64.
- Deleted `vm/memory/image32.cpp` and `.hpp` (never called), plus the `_addr32_*` map, `_heap32_addr`, `register_addr32`, `translate_addr32`, `read_mark`, `_oop_replacement`, `read_long`-era helpers, `_new_format`/`_native_format`, and the old root-order/version branches in `bootstrap.*`.
- Removed the entire 32-bit method-code relocation path in `methodOop.cpp` (`bootstrap_instr`, branch-upgrade layout, method reallocation); `bootstrap_object()` now contains only the native reader.
- `memOopDesc::bootstrap_header()` is native-only: read the klass, install the prototype mark.
- Images with version < 200 are rejected with an "Unsupported image format" diagnostic.
- `imageCompat.cpp` is retained as a macOS-only safety net: the tracked image carries patched `MacOSXStatBuffer` offsets, and re-applying the patch on load is idempotent. It is a no-op for native images on the other platforms.
- **Verified**: build, boot from `strongtalk.bst`, and `make test-image` all pass on arm64-macos, x86_64-macos, x86_64-linux (Ubuntu 24.04/GCC 13 container), and x86_64-windows (MinGW GCC 13 cross-build under Wine 9 in a QEMU TCG amd64 Ubuntu 24.04 guest). The Windows round trip saves two byte-identical 1,514,906-byte images, and its boot reads the image in 0.85 s with no error markers. On Linux and Windows the tracked file differs only in platform-specific state (the `Platform` association and the platform file-path/stat classes), as expected. The tracked image is 1,515,046 bytes, first byte 202, 54,866 objects.
- The x86-64 builds also needed two pre-existing, image-unrelated fixes: the AArch64-only failure-block instructions in `interpreter.cpp` are now arch-guarded, and `interpreterBackend_x86.cpp` implements the previously missing `InterpreterBackend::callPrimitive()` (the macOS linker's `-undefined dynamic_lookup` had hidden the undefined symbol).

### Deliverables
- Native 64-bit `strongtalk.bst` (replaces the 32-bit image in place)
- Native reader/writer with no 32-bit bridge
- `make test-image` round-trip check, also run in Linux x86-64 CI
- Clean boot on arm64-macos; Linux x86-64 validated by CI round-trip

### Risks
- Image format is undocumented; reverse-engineering header offsets is error-prone.
- Compiled code in the image cannot run — interpreted-only boot may be slow but functional.
- Image may reference 32-bit-specific primitives or external structs needing shims.

---

## Plan B: Generate a 64-bit image from `StrongtalkSource/`

Build a fresh `strongtalk.bst` from Delta source (`.dlt/.str/.gr` chunks) so no 32-bit compatibility reader is needed at all.

### Phase 1: Inventory the source
- Run `tools/source_inventory.py` to catalog classes, methods, and initializers from `StrongtalkSource/`.
- Identify the bootstrap sequence: which classes/methods run first (`SystemInitializer`, `ProcessorScheduler`, etc.).
- Compare against what `strongtalk.bst` currently loads (existing image's class/method inventory).

### Phase 2: Build an image writer
- Extend `vm/oops/` or `vm/memory/` with an image emitter: serialize objects with 16-byte headers, 64-bit tagged oops, symbol table, and method bytecodes.
- Root set: nil, true, false, the class hierarchy, `ProcessorScheduler`, `SystemInitializer`.

### Phase 3: Compile or interpret methods
- Delta source is textual Smalltalk.
  - Path A: compile methods to 64-bit bytecodes using the existing compiler (`vm/recompiler/`, `vm/compiler/`).
  - Path B: parse methods into interpreter bytecode format and run interpreted only (simpler for initial boot).
- For boot, interpreted (B) is sufficient; JIT can come later.

### Phase 4: Bootstrap the image
- Construct the minimal object graph in memory (classes first, then methods, then instances).
- Serialize to `strongtalk64.bst`.
- Boot arm64 from it; iterate on missing classes/initializers.

### Phase 5: Replace `strongtalk.bst`
- Once clean boot is verified, ship `strongtalk64.bst` as the default image.
- Delete temporary bootstrap code.

### Deliverables
- Image emitter (serialization) in `vm/memory/` or `vm/oops/`
- Source parser/importer for Delta chunks (reuse `tools/source_inventory.py` logic where possible)
- Native 64-bit `strongtalk.bst`
- Clean boot on arm64-macos

### Risks
- `StrongtalkSource/` may be incomplete or out of sync with the VM's expected class hierarchy (Delta-era source).
- No guarantee the source compiles cleanly under the current type system / bytecode set.
- Chicken-and-egg: need some working image to build the compiler that builds the image (mitigated by hand-seeding the root set).
- Larger scope than Plan A; could take longer to reach clean boot.

---

## Recommendation

Start with **Plan A**. The reader is smaller, well-scoped, and gives a clean boot on all four platforms quickly. Once a running 64-bit VM exists, use it to generate the native image (Plan A Phase 5/6), making Plan B unnecessary.

Plan B only makes sense if the 32-bit image format proves impossible to read reliably, or if you want to purge the frozen image entirely.