# Strongtalk 64-bit Image Boot — Development Ledger

## Current State (as of Thu Nov 05 2026)

### PLAN-64BIT-IMAGE.md Progress
- **Phases 1–4 (COMPLETE)**: The 64-bit VM builds, loads the original 32-bit
  `strongtalk.bst` through the temporary reader bridge, passes GC, and boots
  past class initialization.
- **Phase 5 (COMPLETE)**: Native 64-bit image writer plus `-SaveImage`.
  The writer emits 54,866 objects and re-saving a loaded native image is
  byte-identical.
- **Phase 6 (COMPLETE)**: The tracked `strongtalk.bst` is now the native
  version-202 image, and the 32-bit reader bridge is gone (`image32.cpp/.hpp`,
  `_addr32_*`, old root-order/version branches, and the 32-bit method-code
  relocation path). `make test-image` checks the reader/writer round trip and
  CI runs it on Linux x86-64.

### Files Modified / Added
| File | Status | Changes |
|------|--------|---------|
| `vm/runtime/arguments.cpp` | Modified | `save_image_filename` global + `-SaveImage` parsing |
| `vm/runtime/arguments.hpp` | Modified | Declares `save_image_filename` |
| `vm/runtime/shell.cpp` | Modified | Calls `image_writer_save()` right after `load_image()`, then exits non-zero on failure |
| `vm/runtime/bootstrap.cpp` | Modified | Native-only: rejects version < 200, dependency-order roots, no 32-bit fixups, `?` NULL, `read_native_int()` |
| `vm/runtime/bootstrap.hpp` | Modified | Bridge fields/helpers removed; keeps `read_integer()`/`read_native_int()` |
| `vm/oops/methodOop.cpp` | Modified | Native `bootstrap_object()`; 32-bit relocation path removed |
| `vm/oops/methodOop.hpp` | Modified | Single `bootstrap_object()` declaration |
| `vm/oops/memOop.cpp` | Modified | Native-only `bootstrap_header()` |
| `vm/memory/symbolTable.hpp` | Modified | `firstBucket()`/`lastBucket()` made public |
| `vm/memory/imageWriter.cpp` | Added | Native 64-bit image writer |
| `vm/memory/imageWriter.hpp` | Added | Writer interface / format notes |
| `vm/memory/image32.cpp` / `.hpp` | **Deleted** | Dead 32-bit verification/translation module |
| `vm/interpreter/interpreter.cpp` | Modified | Guarded AArch64-only `tbnz`/`ldr` failure-block sequences for x86-64 |
| `vm/asm/interpreterBackend_x86.cpp` | Modified | Added the missing `callPrimitive()` (`call_C(eax)`); the macOS linker's `-undefined dynamic_lookup` had hidden the undefined symbol |
| `strongtalk.bst` | Modified | Now the native version-202 image (1,515,046 bytes) |
| `Makefile` | Modified | `IMAGE ?= $(ROOT_DIR)/strongtalk.bst`; `test` uses it; `test-image` round-trip target |
| `.github/workflows/linux.yml` | Modified | Runs `make test-image` after `make test` |
| `strongtalk64.bst` | Untracked | Working copy of the same native image; can be removed |

### Native Image Format (version `200 + Bytecodes::version()` = 202)
- The stream starts with a plain base-128 integer version. `>= 200` selects the
  native path in `bootstrap::parse_file()`; older versions are rejected with an
  "Unsupported image format" diagnostic.
- Roots are the existing 26-entry sequence beginning with `first_root()`
  (smiKlass's metaclass); the reader discards root 0 and assigns the rest.
- Objects are written **inline depth-first**. The index is assigned (and the
  object marked written) when its header is emitted, which is exactly when the
  reader's `get_object()` calls `add()` after allocating. Repeated references
  are `'3'` + index. This makes self references and cycles work, including
  `klassKlass`.
- Sizes are native oop counts (`as_mem(obj)->size()`); byte containers need no
  size recomputation.
- Type chars from `format()`:
  - Klasses: `A` klassKlass, `B` smi, `C` mem, `D` byteArray, `E`
    doubleByteArray, `F` objArray, `G` symbol, `H` double, `I` association,
    `J` method, `K` blockClosure, `L` context, `M` proxy, `N` mixin, `O`
    weakArray, `P` process, `Q` doubleValueArray, `R` vframe.
  - Instances: `c` mem, `d` byteArray, `e` doubleByteArray, `f` objArray, `g`
    symbol, `h` double, `i` association, `j` method, `n` mixin, `p` process,
    `q` doubleValueArray.
  - `?` = NULL slot. **Never `N`** — `N` is a valid old-image `mixin_klass`
    type char, which caused a crash when used for NULL.
- Method codes: instruction count, then per instruction the raw opcode/argument
  bytes plus structured inline slots. Branch/failure-block offsets and the DLL
  entry cell are `'I'` + signed integer (DLL entry forced to 0); other slots are
  normal object values. `methodOopDesc::bootstrap_object()` rebuilds the
  native layout byte-for-byte.
- Process pointers are process-specific, so the writer omits them and the
  reader installs NULL.
- Instances of `blockClosure`, `context`, `proxy`, `weakArray`, and `vframe`
  are not serializable (code pointers / stack / foreign addresses) and the
  reader rejects them. They are not reachable in the frozen graph; the writer
  now fails loudly if one ever is.

### Verified
- `make -j8` builds cleanly on arm64-macos.
- The tracked `strongtalk.bst` is byte-identical to the first save of a
  load/save cycle (`make test-image`: 1,515,046 bytes, first byte 202).
- `make test-image` passes on arm64-macos.
- `-b strongtalk.bst` boots with no crash/fatal markers on arm64-macos;
  `[image-compat]` is silent.
- `make test` still hits the pre-existing `Character initialize` failure (see
  below), unrelated to the image format.

Platform matrix for this change:
| Platform | Build | `make test-image` | Boot |
|----------|-------|-------------------|------|
| arm64-macos (clang) | pass | pass, tracked image canonical | pass, scheduler loop, no error markers |
| x86_64-macos (Rosetta) | pass | pass, tracked image canonical | pass, scheduler loop, no error markers |
| x86_64-linux (Ubuntu 24.04, GCC 13.3, Docker) | pass | pass (same-platform round trip) | alive after 12 s, no error markers (killed; SIGTERM triggers the VM register dump) |
| x86_64-windows (MinGW GCC 13, Wine 9 in QEMU TCG amd64 Ubuntu 24.04) | pass | pass (same-platform round trip, 1,514,906 bytes) | alive after 60 s, image read in 0.85 s, no error/AV markers |

The Windows/AMD64 run used the MinGW cross-build from `make all OS=mingw` and
executed `strongtalk.exe` under Wine in an amd64 Ubuntu 24.04 guest emulated by
QEMU TCG (not Rosetta, whose Wine path is known to fault after image read). The
round trip saved the tracked image, reloaded it, saved again, and the two
1,514,906-byte files were byte-identical.

The Linux and Windows saves of the tracked image differ from the tracked file
in platform-specific state (`UnixPlatform` / `Win32FilePath` versus
`MacOSXPlatform` and the macOS stat-buffer classes), which `load_image()`
installs or patches on every load. That is why `test-image` compares two saves
made on the same platform instead of comparing against the tracked file.

### Windows/AMD64 validation harness
The MinGW build is produced with
`make -j$(nproc) all OS=mingw CXX=x86_64-w64-mingw32-g++`. Because Wine under
Rosetta faults after image read (`invalid gdt selector index 5`), the run used an
amd64 Ubuntu 24.04 cloud image booted with QEMU TCG, with the build directory
shared over 9p and Wine 9 installed in the guest. Cloud-init seeds were served
with `-smbios 'type=1,serial=ds=nocloud-net;s=http://10.0.2.2:8000/'` because
macOS `hdiutil` ISO labels are uppercase (`CIDATA`) while the NoCloud datasource
looks for lowercase `cidata`. The guest saved the image twice and compared the
two files byte-for-byte.

### What's Broken / Remaining
1. `make test` still hits the pre-existing `initializeSmalltalkEnvironment()` →
   `Smalltalk initialize` → `Character initialize` →
   `initializeClassifications` failure. This is unrelated to the image format.
2. Windows x86-64 was validated locally with a MinGW cross-build under Wine in
   a QEMU TCG amd64 guest. The native `windows-latest` CI job has not run this
   change yet (it needs the commit pushed).
3. The macos/amd64 support needed two pre-existing fixes that were unrelated to
   the image work: the unguarded AArch64 failure-block instructions in
   `interpreter.cpp`, and the missing x86-64 `InterpreterBackend::callPrimitive`.
4. `imageCompat.cpp` is retained as a macOS-only safety net. The tracked image
   carries patched `MacOSXStatBuffer` offsets; re-applying the patch on load is
   idempotent, and it is a no-op on other platforms. It can be retired once the
   image is regenerated from Delta source or those offsets stop mattering.
5. The interactive prompt is hard to observe under this harness (the scheduler
   watcher keeps a CPU busy in both the old and native images); parity with the
   original image is the current boot criterion.
6. `strongtalk64.bst` (untracked) is now redundant with `strongtalk.bst` and can
   be removed once the user confirms.

## History / Superseded Approaches (do not retry)
- **Pre-assigned indices + forward refs**: wrote `ref idx` for an object that
  had an index but had not yet been allocated by the reader, causing
  "bootstrap oop table overflow". Fixed by assigning the index only at header
  emission and writing every first reference inline.
- **Root table / dependency sorting**: unnecessary once inline DFS matches the
  reader's allocation order.
- **Traversing objArray elements separately**: unnecessary; the DFS writes
  indexed elements as part of the object's field stream.
- **`N` as the NULL token**: collided with the old image's `mixin_klass` type
  char. Use `?`.
- **32-bit size recomputation in native mode**: must be skipped; native sizes
  are already in oops.
- **Keeping the 32-bit bridge "just in case"**: the native image is now the
  only supported format; loading anything older is rejected.

## Useful Commands
```
# Build
make -j8

# Read/write round trip on the tracked image (same platform)
make test-image

# Save a native 64-bit image from the loaded image
DYLD_LIBRARY_PATH=build/arm64-macos-clang ./build/arm64-macos-clang/strongtalk \
  -b strongtalk.bst -SaveImage strongtalk64.bst

# Boot from the native image
DYLD_LIBRARY_PATH=build/arm64-macos-clang ./build/arm64-macos-clang/strongtalk \
  -b strongtalk.bst
```

### Notes on Two File Paths That Must Stay in Sync
`oop_slot_layout()` in `vm/memory/imageWriter.cpp` duplicates
`oop_instr_offsets()` in `vm/oops/methodOop.cpp`. They describe where the
interpreter keeps inline oop slots; change them together.
