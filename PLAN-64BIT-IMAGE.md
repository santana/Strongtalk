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

### Phase 2: Fix field offset translation during bootstrap (IN PROGRESS)
- **Confirmed root cause**: **0 bad objects** but **54,968 bad fields** across ALL klass types — field-level 4-byte→8-byte misalignment WITHIN objects, not header layout.
- **Affected klass types**: Method (19K), Array (15.5K), CompressedSymbol (14K), GlobalAssociation (1.4K), Metaclass (1.1K), Float (280), Character (257), plus many others with 1-2 fields each.
- **Mechanism**: The 32-bit image writes instVar fields at 4-byte offsets; the 64-bit VM's `raw_at(i)` reads them at `i * 8` byte offsets. Pairs of 32-bit fields merge into one 64-bit word, producing garbage tagged oops (raw integers, ASCII text, Mark_Tag values).
- **Fix location**: `vm/runtime/bootstrap.cpp` `get_object()` — after each object's type-specific `bootstrap_object()` completes, walk oop fields 0..non_indexable_size() and widen any raw 32-bit value to a 64-bit tagged oop.
- **Key insight**: `read_oop()` reads type-tagged oops (already 64-bit via oop_table), but raw 32-bit instVar fields stored inline in the image bypass `read_oop()` and need manual widening.
- **Smi widening**: 30-bit smi → 62-bit smi (tag stays Int_Tag=0, value shifted).
- **Mem widening**: 32-bit raw heap pointer → 64-bit tagged oop (tag Mem_Tag=1, pointer widened).
- **Null smi (0)** stays valid — `smiOop_zero` is already 0.
- **Already integrated**: `image32_verify_old_gen()` called in `load_image()` after `imageCompat` patches.

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
