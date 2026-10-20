# 64-bit Image Boot Plan

Goal: get macOS arm64 (and Linux x86-64, macOS x86-64, Windows x86-64) past `Eval>` to a clean boot (`ProcessorScheduler>>start` → `SystemInitializer` → `self run`).

Current state: all four configs build, load `strongtalk.bst`, pass GC, and reach `Eval>` — but `Eval>` is the error-fallback REPL, not a successful boot. The shared blocker is the interpreter's receiver-slot decode passing non-pointers (nil/smi/garbage) to `InterpretedIC::inline_cache_miss()`. Root cause under investigation: `strongtalk.bst` was written by a 32-bit VM (8-byte object header: mark+klass) while the VM is 64-bit-only (16-byte header), so field offsets read from the image are misaligned, producing bogus receivers.

Two approaches:

---

## Plan A: Temporary 32-bit image reader bridge

Read the 32-bit `strongtalk.bst` with 32-bit header offsets, re-header objects into the 64-bit layout in memory, boot, then regenerate a native 64-bit image and delete the reader. 32-bit knowledge is isolated to image read-in only — not the object model, assembler, or runtime.

### Phase 1: Map the 32-bit object header (COMPLETE — audit done)
- **Image format**: text-based, not raw heap dump. `vm/runtime/bootstrap.cpp` reads opcodes (`'0'` smi, `'-'` neg smi, `'3'` ref, `'c'` memOop, `'d'` byteArray, `'e'` doubleByteArray, `'f'` objArray, `'g'` symbol, `'h'` double, `'i'` association, `'j'` method, `'n'` mixin, `'p'` process, etc.) and allocates via `Universe::allocate_tenured(size)`.
- **Header**: `memOopDesc::bootstrap_header()` reads mark(4)+klass(4) for old format; new format reads klass only. 64-bit layout: mark(8)+klass(8) = 16 bytes.
- **klassOop**: `bootstrap_object()` reads `_non_indexable_size`, `_has_untagged_contents`, header, then klass fields (`_classVars`, `_methods`, `_superKlass`, `_mixin`).
- **methodOop**: bootstrap already handles 32→64-bit bytecode relocation (byte items advance 1, oop items advance 4 in 32-bit; oops are 8 bytes in 64-bit, interpreter 8-byte aligns inline oops). Uses `bootstrap_instr`, `rec_w32`/`rec_w64` arrays.
- **doubleOop** (`'h'`): reads `read_double()` (8 raw bytes) — OK on both.
- **Byte containers** (`'d'`, `'g'`, `'e'`): size already recomputed from 32-bit oop count (4 bytes each) to 64-bit oop count (8 bytes each).
- **CRITICAL GAP**: No size recomputation covers object headers or klass `_non_indexable_size`. A 32-bit object with N instVars stores them at 4 bytes each, but `allocate_tenured(size)` with the image's `size` field under-allocates on 64-bit (needs 8 bytes per field).
- **`imageCompat.cpp`**: existing load-time shim rewrites `Alien>>ensureLoaded:` (size 4→8) and `primitiveNew:` operand (3→15) so the frozen 32-bit image allocates the 16-byte LP64 header. This is separate from header layout but complementary — keep it.

### Phase 2: Build the 32-bit→64-bit reader
- Add `vm/memory/image32.cpp` (+ `.hpp`): opens `strongtalk.bst`, parses 32-bit headers, materializes objects in the 64-bit heap with correct tagging (`Mem_Tag`, `Int_Tag`) and 8-byte fields.
- Oop widening: every 32-bit raw oop → 64-bit tagged oop; Smi values widened from 30-bit to 62-bit range.
- Rebase all object pointers; re-hash symbol/intern tables.
- Plug into `Universe::load_image()` / `vm/runtime/imageCompat.*` path so boot uses it. Keep the existing Alien load-time rewrite (separate from header layout).

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
