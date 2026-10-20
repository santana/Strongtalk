# 64-bit Image Boot Plan

Goal: get macOS arm64 (and Linux x86-64, macOS x86-64, Windows x86-64) past `Eval>` to a clean boot (`ProcessorScheduler>>start` → `SystemInitializer` → `self run`).

Current state: all four configs build, load `strongtalk.bst`, pass GC, and reach `Eval>` — but `Eval>` is the error-fallback REPL, not a successful boot. The shared blocker is the interpreter's receiver-slot decode passing non-pointers (nil/smi/garbage) to `InterpretedIC::inline_cache_miss()`. Root cause under investigation: `strongtalk.bst` was written by a 32-bit VM (8-byte object header: mark+klass) while the VM is 64-bit-only (16-byte header), so field offsets read from the image are misaligned, producing bogus receivers.

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

### Phase 3: Compiled-code problem
- 32-bit machine code inside methodOops will not execute on a 64-bit CPU.
- Choose simplest path for the bridge: mark all compiled methods for interpretation/recompilation on first send (force interpreted execution until the JIT recompiles hot methods).

### Phase 4: Boot and validate
- Build arm64-macos (and x86-64 for cross-check).
- Run until `ProcessorScheduler>>start` completes → clean boot, not `Eval>`.
- Fix residual `nil doesNotUnderstand:` cascades from misread fields.

### Phase 5: Write a native 64-bit image
- Add a save primitive / VM option to dump the running 64-bit heap to `strongtalk64.bst` with 16-byte headers.
- Boot from that image to prove round-trip.

### Phase 6: Remove the bridge
- Delete `image32.cpp` and any 32-bit read-in code once a 64-bit image exists.

### Deliverables
- `vm/memory/image32.cpp` (+ `.hpp`) — temporary reader
- 64-bit `strongtalk64.bst` image
- Clean boot on arm64-macos and x86-64 (no `Eval>`)
- Removal commit

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
