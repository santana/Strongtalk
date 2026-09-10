# Strongtalk VM Architecture

## Overview

Strongtalk is an optionally-typed Smalltalk with a high-performance optimizing
JIT compiler, developed by LongView Technologies LLC (1994-1997) and
open-sourced by Sun Microsystems in 2006. The VM is written in C++ and
self-hosts its own optimizing compiler and garbage collector.

The system has three major layers:

1. **Object System** -- tagged pointers, object headers, class hierarchy
2. **Execution Engine** -- interpreter, optimizing compiler, inline caches
3. **Runtime Services** -- garbage collection, process scheduling, primitives

---

## 1. Object System (Oops)

Strongtalk uses a tagged-pointer representation. Every `oop` (ordinary object
pointer) is a machine word with the low two bits encoding its type:

| Tag (2 bits) | Type    | Description                         |
|-------------|---------|-------------------------------------|
| `00`        | `Int`   | Small integer (Smi) -- value in upper bits |
| `01`        | `Mem`   | Heap-allocated object               |
| `10`        | (unused) |                                     |
| `11`        | `Mark`  | Mark word (used during GC)          |

```cpp
// vm/topIncludes/tag.hpp
const int Int_Tag  = 0;  // Smi
const int Mem_Tag  = 1;  // heap object
const int Mark_Tag = 3;  // GC mark
```

**Immediate values**: Smis encode the integer directly in the pointer (`Smi(n) = n << 2`).
No heap allocation is needed.

**Heap objects**: `oopDesc*` pointers are untagged C pointers to the raw memory.
`oop` (typedef for `oopDesc*`) pointers are tagged. Conversion:
- `oop` -> raw pointer: `memOop(oop)->addr()` subtracts `Mem_Tag`
- raw pointer -> `oop`: `as_memOop(ptr)` adds `Mem_Tag`

On AArch64, `oopSize = 8` and `slotSize = 2*oopSize = 16` (each stack slot is
16 bytes to maintain alignment).

### Object Hierarchy

```
oopDesc                          (base: just a mark word)
  smiOopDesc                     (not a real object; encoding only)
  memOopDesc                     (mark + klass pointer)
    associationOopDesc           (name -> value binding)
    blockClosureOopDesc          (block context)
    byteArrayOopDesc             (byte-indexed array)
      symbolOopDesc              (interned symbol)
    doubleByteArrayOopDesc       (8-bit-element array)
    doubleOopDesc                (boxed double)
    doubleValueArrayOopDesc      (array of doubles)
    klassOopDesc                 (class object)
    methodOopDesc                (compiled method)
    mixinOopDesc                 (mixin for mixins-based inheritance)
    objArrayOopDesc              (oop-indexed array)
      weakArrayOopDesc           (weak references)
    processOopDesc               (Smalltalk process)
    proxyOopDesc                 (external object)
    vframeOopDesc                (virtual frame for debugging)
```

### Klass Structure

Each class has a `Klass` C++ object (not a heap object) that holds:

```
Klass layout:
  [vtbl                  ]   C++ virtual dispatch table
  [non_indexable_size    ]   size of fixed (non-indexable) part
  [has_untagged_contents ]   whether body contains raw bytes
  [classVars             ]   class variables (copied from mixin)
  [methods               ]   customized methods
  [superKlass            ]   superclass
  [mixin                 ]   the mixin this class is derived from
```

Klasses are themselves heap objects (`klassOopDesc` wraps a `Klass` inside a
`memOopDesc`). Every heap object points to its klass via the `_klass_field`
in the `memOopDesc` header.

### Method Layout

A `methodOopDesc` contains:

```
[_mark           ]   object header (GC info)
[_klass_field    ]   pointer to methodKlass
[_debugInfo      ]   debug info (objArrayOop)
[_selector_or_method] selector for normal methods, enclosing method for blocks
[_counters        ]   invocation counter + sharing counter
[_size_and_flags  ]   [flags:8 | nofArgs:4 | size:18 | tag:2]
[bytecodes...    ]   the method's bytecodes follow inline
```

---

## 2. Execution Engine

### 2.1 Interpreter

The interpreter is a **threaded-code** interpreter generated at boot time by
`InterpreterGenerator`. Each bytecode has a dedicated assembly-language
handler. Dispatch is via a computed-goto scheme (jump through a register on
x86-64/AArch64).

**Key registers** (AArch64):

| Register | Name    | Purpose                        |
|----------|---------|--------------------------------|
| `x13`    | `eax`   | Top of stack (TOS)             |
| `x14`    | `esi`   | Bytecode pointer               |
| `x15`    | `ebx`   | Dispatch table base / next bytecode |
| `x29`    | `ebp`   | Frame pointer                  |
| `sp`     | `esp`   | Stack pointer                  |
| `x16`    | `ecx`   | Scratch / used by send logic   |
| `x12`    | `edx`   | Scratch                        |
| `x27`    | `edi`   | Dispatch table register        |
| `x11`    | (scratch)|                               |

**Frame layout** (AArch64, from frame pointer):

```
fp[-6]: temp area
fp[-4]: hp (heap pointer for allocation)
fp[-2]: receiver (self)
fp[ 0]: link (previous fp)
fp[ 1]: return address
fp[ 2]: arg_n_offset -- first argument / sender sp
fp[ 3]: second argument
...
```

**Bytecode set** (256 codes, version 2). Every opcode is a 1-byte `def()` call
in `vm/interpreter/bytecodes.cpp` carrying an operand *format* and one of 14
*code types* (local/instVar/classVar/global/context access, closure/context
creation, control flow, message sends, non-local returns, primitive calls,
DLL calls, float ops, miscellaneous). The authoritative reference is the
developer guide `BYTECODES.md` at the repo root (17 operand formats, the
complete opcode table, inline-cache transitions, and interpreter
architecture); a generated HTML rendering lives at
`documentation/internal/vm/bytecodes.html` and is refreshed with `make docs`.

Send bytecodes come in multiple specializations: interpreted, compiled,
primitive, accessor, polymorphic, megamorphic. The interpreter picks the
appropriate variant based on inline cache state at method entry.

### 2.2 Inline Caches and Lookup

Every send site has a **compiled inline cache** (`CompiledIC`) consisting of:

```
  -5: call  _icLookupStub   (unfilled)
           | nmethod addr   (filled with compiled method)
           | pic entry      (filled with PIC)
   0: test reg,             (dummy instruction)
   1: ic_info               (NLR offset + flags in imm32)
   5: ...
```

**Lookup resolution**:
1. **Unfilled**: call goes to `lookupCache` stub, which resolves the method
2. **Monomorphic**: call goes directly to a single nmethod
3. **Polymorphic**: call goes to a **Polymorphic Inline Cache** (PIC) that
   dispatches based on receiver class
4. **Megamorphic**: falls back to a class-based lookup table (stride-based
   hash table keyed on receiver class)

The lookup cache (`vm/lookup/`) uses a two-level scheme:
- **entry**: receiver klass + method holder klass -> nmethod*
- **ntable**: nmethod* -> nmethod*

Cache invalidation happens when methods are recompiled, classes change, or
nmethods become zombies.

### 2.3 Optimizing Compiler (Delta Compiler)

The compiler (`vm/recompiler/`) is a recompiling JIT. It compiles frequently
executed methods from bytecodes to native machine code (nmethods).

**Compilation pipeline**:

1. **Method invocation** triggers `invocation_counter` increment
2. When threshold is reached, the method is queued for **recompilation**
3. The **recompiler** (`recompiler.cpp`) compiles the method:
   - Builds an **inline cache** tree from the bytecodes
   - Performs **type inference** using inline caches
   - Generates native code through the assembler backend
4. The generated **nmethod** is installed, replacing the interpreter entry

**nmethod structure** (`vm/code/nmethod.hpp`):

```
  [header                ]   version, optimization level, age, state flags
  [entry points          ]   entry_point, verified_entry_point, special_handler_call
  [machine instructions  ]   native code (NCodeBase)
  [oop map information   ]   which stack slots contain oops
  [scope descriptions    ]   inlining tree for deoptimization
  [PC descriptions       ]   maps native PCs back to bytecodes
  [debugging information ]   for the debugger
  [dependency info       ]   which assumptions are violated
```

**nmethod states**: `alive` -> `zombie` -> `dead`

**Optimization levels** (0-15, stored in `nmFlags.level`): higher levels
apply more aggressive optimizations.

### 2.4 Deoptimization

When assumptions made by the compiler are invalidated (e.g., a class is
subclassed, an inline cache becomes megamorphic), the VM can **deoptimize** a
compiled frame back to interpreted execution:

1. The nmethod is marked for deoptimization
2. At the next safepoint, the stack frame is reconstructed using the scope
   descriptions (`scopeDesc`) stored in the nmethod
3. The frame is replaced with a `deoptimizedFrame` containing a `vframeArray`
   that holds the interpreter state
4. Execution resumes in the interpreter

### 2.5 Assembler Backends

The assembler (`vm/asm/`) provides an architecture-independent interface with
two backends:

| Backend            | Files                          | Status     |
|--------------------|--------------------------------|------------|
| x86-64 (default)   | `assembler_x86.hpp/cpp`       | Complete   |
| AArch64 (ARM64)    | `assembler_aarch64.hpp/cpp`   | Complete   |

Backend selection is at compile time via `DELTA_ASSEMBLER_BACKEND_AARCH64`:

```cpp
// vm/asm/assembler.hpp
#if defined(DELTA_ASSEMBLER_BACKEND_AARCH64)
  typedef AArch64Assembler Assembler;
  typedef AArch64MacroAssembler MacroAssembler;
#else
  typedef X86Assembler Assembler;
  typedef X86MacroAssembler MacroAssembler;
#endif
```

Each backend implements:
- **Assembler**: raw instruction emission (`mov`, `add`, `ldr`, `str`, etc.)
- **MacroAssembler**: high-level operations (`call_C`, `store_check`,
  `dispatch_base`, `align_stack_before_call`)
- **Mapping**: register allocation conventions (which registers map to
  which VM roles)

---

## 3. Memory Management

### 3.1 Heap Organization

The heap is divided into two generations:

```
+-------------------+-------------------+
|    New Generation  |   Old Generation  |
|                   |                   |
|  +-----+---------+|                   |
|  | Eden|  From   ||   Old Space       |
|  |     |  To     ||   (card-marked)   |
|  +-----+---------+|                   |
+-------------------+-------------------+
```

**New Generation** (`newGeneration`):
- **Eden**: bump-pointer allocation of new objects
- **From Space**: survivor space (objects that survived last scavenge)
- **To Space**: target of scavenge (swap with From after scavenge)

**Old Generation** (`oldGeneration`):
- **Old Space**: long-lived objects, promoted from new generation
- Uses **card marking** via `rSet` (remembered set) to track cross-generation references

### 3.2 Garbage Collection: Scavenge

The primary GC is a **copying semi-space collector** (Cheney's algorithm):

1. **Trigger**: eden fills up (`NeedScavenge` flag set)
2. **VMProcess::execute(VM_Scavenge)** is called to start collection
3. **Roots scanning**: `Universe::oops_do(scavenge_oop)` walks all known roots
4. **Frame scanning**: `Processes::scavenge_contents()` walks all Delta thread
   stacks, scavenging oops found in frame slots
5. **Copy**: surviving objects in `from_space` are copied to `to_space`
6. **Promotion**: objects that survive multiple scavenges are promoted to old space
7. **Swap**: `from_space` and `to_space` are swapped
8. **Tenuring**: the `age_table` tracks object ages; objects exceeding
   `tenuring_threshold` are promoted

The scavenge process is incremental -- promoted objects in old space are
scanned via `old_gen.scavenge_contents_from(&mark)` until no more new
references are found.

### 3.3 Garbage Collection: Mark-Sweep

A full **mark-sweep** collector (`vm/memory/markSweep.cpp`) handles old-space
collection:

1. Mark all reachable objects starting from roots
2. Sweep dead objects, reclaiming space
3. Used less frequently than scavenge

### 3.4 Weak References

The `WeakArrayRegister` tracks weak arrays. During scavenge,
`WeakArrayRegister::check_and_scavenge_contents()` clears references to
dead objects.

### 3.5 Memory Allocation

```cpp
// vm/memory/universe.hpp
static oop* allocate(int size, memOop* p = NULL, bool permit_scavenge = true);
```

- If `permit_scavenge` is true and eden is full, triggers scavenge
- If `permit_scavenge` is false, returns NULL if no space
- `allocate_tenured(size)` allocates directly in old space

---

## 4. Runtime Services

### 4.1 Process Model

The VM has a cooperative multitasking model:

- **VMProcess**: singleton process that handles VM operations (scavenge, GC,
  sweeper). Runs in its own native thread.
- **DeltaProcess**: represents a Smalltalk process. Each DeltaProcess has:
  - A native thread
  - A Delta stack (C stack + interpreter frames)
  - A `last_frame` pointer
  - An `unwinder` chain for non-local returns

**Process states**: `running`, `yielded`, `stopped`, `preempted`, `completed`,
`aborted`, etc.

**VM Operations** (`vm/runner/vmOperations.hpp`): The VM synchronizes via
`VMProcess::execute(op)`:
- `VM_Scavenge` -- garbage collection
- `VM_GZip` -- memory compaction
- `VM_List` -- debugging
- `VM_Compact` -- heap compaction
- `VM_ZapResourceArea` -- debug cleanup

### 4.2 Primitives

Primitives are built-in operations implemented in C++ (`vm/prims/`).

**Primitive families**:

| File                         | Purpose                           |
|------------------------------|-----------------------------------|
| `smi_prims.cpp`              | Small integer operations          |
| `double_prims.cpp`           | Floating-point operations         |
| `objArray_prims.cpp`         | Array access and manipulation     |
| `byteArray_prims.cpp`        | Byte array operations             |
| `behavior_prims.cpp`         | Class behavior (class creation, etc.) |
| `method_prims.cpp`           | Method introspection              |
| `block_prims.cpp`            | Block closure operations          |
| `mixin_prims.cpp`            | Mixin operations                  |
| `process_prims.cpp`          | Process scheduling                |
| `system_prims.cpp`           | System-level operations (GC, etc.)|
| `oop_prims.cpp`              | Object identity, copying          |
| `debug_prims.cpp`            | Debugger support                  |
| `dll.cpp`                     | Foreign function interface        |

**Primitive dispatch**: The interpreter calls `primitive(int index)` which
looks up the primitive in the primitive table. Each primitive can succeed
(return an oop), fail (return a failure code), or call back into Smalltalk.

**Generated primitives** (`prims.src`, `prims.inc`): Some primitives are
generated from a specification file for performance.

### 4.3 Non-Local Returns (NLR)

Strongtalk uses a setjmp/longjmp-like mechanism for non-local returns:

1. The home context is captured on block creation
2. On NLR, the VM walks up the frame chain looking for the matching context
3. Frames are unwound via `unwindInfo` chain
4. The result is placed in `_nlr_result` and control transfers to the home method

NLR info is encoded in inline cache `ic_info` fields (signed 24-bit offset
from return address to NLR test point).

### 4.4 Sweeper

The sweeper (`vm/runtime/sweeper.cpp`) runs in the background:
- Zombifies dead nmethods (those with invalidated dependencies)
- Manages nmethod age for recompilation decisions
- Triggers recompilation of hot methods

### 4.5 Deoptimization and Recompilation

When a method is recompiled:
1. Old nmethods become zombies
2. Any compiled frames executing zombie nmethods are deoptimized
3. The interpreter resumes execution of those frames
4. The new nmethod is installed and takes over on next call

---

## 5. Source Organization

```
vm/
  asm/              Assembler backends (x86-64, AArch64)
  code/             Compiled code management (nmethods, inline caches,
                    PICs, code table, relocation, stub routines)
  compiler/         (absorbed into recompiler/)
  disasm/           Disassembler for debugging
  interpreter/      Bytecode interpreter (code generation, bytecodes)
  lookup/           Method lookup and inline cache resolution
  memory/           Heap management (universe, spaces, generations,
                    GC, handle management, symbol table)
  oops/             Object representations (oop hierarchy, klass system)
  prims/            Built-in primitives
  recompiler/       Optimizing JIT compiler and recompilation
  runtime/          Core runtime (processes, frames, stack chunks,
                    VM operations, OS abstraction, debugging)
  topIncludes/      Fundamental types and constants (tags, bits)
  utilities/        Utility classes (arrays, hash tables, etc.)

build/              Out-of-tree per-config build dirs (build/<arch>-<os>-<compiler>)
test/               C++ test suite
easyunit/           Lightweight test framework
source/             Smalltalk library source
documentation/      Original HTML documentation (typed-Smalltalk papers,
                    primitives); generated bytecode reference and original
                    Digitalk bytecode table live in documentation/internal/vm/
                    (bytecodes.html, bctable.pdf)

Repo-root developer docs:
  BYTECODES.md           Authoritative bytecode developer guide
                         (formats, opcode table, inline-cache transitions)
```

---

## 6. Platform Support

| Platform              | Build  | Runtime state  |
|-----------------------|--------|---------------------------------------------------------------------------|
| macOS arm64 (native)  | Yes    | Boots, loads image; hits the `zone.cpp:622` `methodHeap->contains()` assert (last blocker) |
| macOS x86-64 (forced) | Yes    | Boots startup; SIGSEGVs at boot end (same signature as the shared-dir builds) |
| Linux x86-64 (Docker) | Yes    | Boots, loads image; `stest` spins in `os::suspend_thread`/`os_dump_context` (wait-stub) |
| Windows x86           | legacy | `build.win32` (Visual Studio) project still in tree; 32-bit x86 was dropped from the C++ sources -- not maintained |

### Platform Abstraction

OS-specific code is isolated in:
- `vm/runtime/os_darwin.cpp` -- macOS (signals, mmap, threads, SIGILL handler + AArch64 crash-dump block)
- `vm/runtime/os_linux.cpp` -- Linux
- `vm/runtime/os_nt.cpp` -- Windows

The `VirtualSpace` class (`vm/runtime/virtualspace.cpp`) abstracts virtual
memory allocation across platforms.

---

## 7. Key Design Patterns

### Tagged Pointers with Klass Dispatch

Objects don't contain a C++ vtable pointer. Instead, all "virtual" operations
forward through the `Klass`:

```cpp
// oopDesc forwards to Klass via blueprint()
oopDesc::blueprint() -> Klass* klass
Klass provides: oop_size(), oop_is_*(), oop_scavenge(), etc.
```

This keeps object headers small (just mark + klass pointer) while still
allowing type-specific behavior.

### Handle-Based GC Safety

When C++ code holds oop pointers across potential GC points, it must use
`Handle` objects:

```cpp
Handle h(some_oop);
// ... GC may occur here ...
oop result = h.obj();  // safe: Handle is updated by GC
```

### VM Operation Protocol

All GC and heap-modifying operations go through `VMProcess::execute()`:

```cpp
VM_Scavenge op(&rec);
VMProcess::execute(&op);
// op is complete; DeltaProcess was suspended during execution
```

This ensures the VM is in a consistent state during heap mutations and that
all thread stacks are properly scanned.

### Memory Barriers and Store Checks

The `STORE_OOP` macro handles write barriers:

```cpp
void set_superKlass(klassOop super) {
  STORE_OOP(&_superKlass, super);  // card-mark if writing to old space
}
```

---

## 8. Build System

The build uses GNU Make with the portable `Makefile` at the repo root. Every
configuration builds into its own out-of-tree directory,
`build/<arch>-<os>-<compiler>` (e.g. `build/arm64-macos-clang`):

- **Auto-detection**: `uname -m` selects arm64 vs x86-64
- **Defines**: `-DDELTA_COMPILER -DASSERT -DDEBUG` (debug build)
- **AArch64**: adds `-DDELTA_ASSEMBLER_BACKEND_AARCH64`
- **Object files**: `vm/**/*.cpp` -> `<build>/obj/vm/**/*.o`
- **Shared library**: `strongtalk.so` (all objects except `main.o`)
- **Binaries**: `strongtalk` (VM), `stest` (test runner)

```sh
make              # build into build/<arch>-<os>-<compiler>
make vm           # build just the strongtalk VM
make test         # run C++ test suite (stest -b strongtalk.bst)
make docs         # regenerate documentation/internal/vm/bytecodes.html
make clean        # remove that config's build directory
make pristine     # like clean, also removes that config's .d dependency files
make format-check # verify clang-format conformance
make ARCH=x86_64  # force the x86-64 backend
make CXX=g++      # pick a different compiler (changes the build-dir token)
make BUILD_DIR=/path  # place the build anywhere
make -j$(nproc)   # parallel build
```

`CC`/`CXX`/`ARCH` default to the host (`cc`/`c++`, `uname -m`). Each config's
objects/deps mirror the source tree under `<BUILD_DIR>/obj/`, so configs never
share objects and switching needs no `clean`. The x86-64 backend is also
exercised on real glibc via an amd64 Docker container:

```sh
docker run --rm --platform linux/amd64 \
  -v "$PWD":/src -w /src \
  strongtalk:linux-tools make
```

The Makefile exports the runtime library path (`DYLD_LIBRARY_PATH` on macOS,
`LD_LIBRARY_PATH` on Linux) for `make test` and `make docs`.

---

## 9. Key Constants

| Constant          | Value (AArch64) | Purpose                        |
|-------------------|-----------------|--------------------------------|
| `oopSize`         | 8               | Size of one oop in bytes       |
| `slotSize`        | 16              | Size of one stack slot (2*oopSize) |
| `Tag_Size`        | 2               | Bits used for tag              |
| `Mem_Tag`         | 1               | Tag for heap objects           |
| `Int_Tag`         | 0               | Tag for Smis                   |
| `EdenSize`        | 512             | Eden in KB (configurable)      |
| `oopsPerSlot`     | 2               | oops per stack slot on AArch64 |
| `arg_n_offset`    | 16              | Byte offset to first arg from fp |
