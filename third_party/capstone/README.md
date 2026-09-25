# Vendored Capstone 5 (disassembler engine)

This tree is the **Capstone disassembler engine, version 5.0.9**, vendored
from https://github.com/capstone-engine/capstone (BSD-licensed; see
`LICENSE.TXT` and `LICENSE_LLVM.TXT`).

It provides the single disassembler used by the VM's `+PrintAssemblyCode`
debug facility (`vm/disasm/disassembler.cpp`) on **all** supported platforms
(macOS arm64, macOS x86-64, Linux x86-64, Windows x86-64/MinGW), replacing
the old platform-specific dynamic load of `libnasm`. The choice of engine was
prompted by `libnasm` not being uniformly available.

## What was kept

Enough of the source tree to build `cs.c` with the two architectures the VM
uses, in the C language (the VM builds it with `$(CXX) -x c` so the objects
always match the C++ toolchain):

- top-level: `cs.c`, `Mapping.c`, `MCInst.c`, `MCInstrDesc.c`,
  `MCRegisterInfo.c`, `SStream.c`, `utils.c` and their headers;
- `arch/X86/*.c` (full instruction tables) and `arch/AArch64/*.c`;
- every other `arch/<name>/` directory reduced to its `<Name>Module.h` stub
  only, because `cs.c` unconditionally includes all `arch/*/*Module.h`;
- `include/capstone/*.h`.

## What was trimmed

- `bindings/`, `contrib/`, `docs/`, `suite/`, `tests/`, `msvc/` and other
  non-core directories;
- the top-level X86 table *reduce* build mode
  (`CAPSTONE_X86_REDUCE`) is **not** used: the reduced tables drop the MPX
  cases (`X86_BND*` operands) that the full disassembler still references,
  which fails to compile with `-DCAPSTONE_*_REDUCE`;
- the exported API is a subset, but `cs.c`'s unconditional `arch/*/*Module.h`
  includes mean a future engine addition needs only its source files.

## Build flags used by the repository

Both architectures are always enabled (`-DCAPSTONE_HAS_X86
-DCAPSTONE_HAS_ARM64`) so one object set serves every config; `cs_open`
selects the engine at run time from the `DELTA_BACKEND_*` macro in
`vm/disasm/disassembler.cpp`. Capstone 5 also requires `cs_option(CS_OPT_MEM)`
to be set **before** the first `cs_open()` — the VM wires its libc allocator
in `disassembler.cpp`. (Failure to do so yields `CS_ERR_MEMSETUP`.)

## Where it is built

- the main `Makefile` compiles the sources out-of-tree into
  `$(BUILD_DIR)/obj/third_party/capstone/...`, archives them into
  `$(BUILD_DIR)/libcapstone.a`, and links that archive into `strongtalk.so`/
  `stest.so`;
- `test/assembler/Makefile` compiles the same sources into its own `objs/`
  directory for the standalone disassembler round-trip tests.