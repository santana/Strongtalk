# Strongtalk

[![macOS build](https://github.com/santana/Strongtalk/actions/workflows/macos.yml/badge.svg)](https://github.com/santana/Strongtalk/actions/workflows/macos.yml)
[![Linux build](https://github.com/santana/Strongtalk/actions/workflows/linux.yml/badge.svg)](https://github.com/santana/Strongtalk/actions/workflows/linux.yml)
[![Windows build (MinGW cross)](https://github.com/santana/Strongtalk/actions/workflows/windows-mingw.yml/badge.svg)](https://github.com/santana/Strongtalk/actions/workflows/windows-mingw.yml)
[![Windows build (native MSYS2)](https://github.com/santana/Strongtalk/actions/workflows/windows-mingw-native.yml/badge.svg)](https://github.com/santana/Strongtalk/actions/workflows/windows-mingw-native.yml)

An optionally-typed Smalltalk with a high-performance optimizing JIT, developed
by LongView Technologies LLC (1994-1997) and open-sourced by Sun Microsystems in
2006. The VM is written in C++ and self-hosts its own optimizing compiler and
garbage collector.

## Status

The VM is a work in progress as a project. The C++ code builds on **Linux
(x86-64)**, **macOS (Apple Silicon)**, and **Windows (x86-64, MinGW cross-built
and native MSYS2)** via the portable root `Makefile` (out-of-tree builds), and
a CI build runs on every push.

The JIT/code generator has a **single frontend with per-architecture
backends**: it emits **x86-64 machine code** on x86-64 and **AArch64 machine
code** on Apple Silicon. The AArch64 backend is ported and exercising the full
JIT pipeline (compiler, scope-description recording, inline caches, jumps,
deoptimization, and recompilation): the VM boots, the image read-in completes
fully, JIT-compiled frames install and run, and recompile/deopt cycles execute.
It currently aborts during a hot-method recompile on
`assert(methodHeap->contains(n), "not in zone")` in `zone::findNMethod`
(`zone.cpp:622`), the active blocker.

| Platform                  | Build  | Runtime                                                          |
| ------------------------- | ----- | ---------------------------------------------------------------- |
| Linux x86-64 (native)     | yes   | loads the image, then spins in the interpreter bootstrap loop (repeated `error:` re-raise in `runBaseClassInitializers`); no JIT code yet |
| macOS arm64 (AArch64)     | yes   | boots, loads the image, runs JIT-compiled code; blocked at a `findNMethod` "not in zone" assert (`zone.cpp:622`) during recompile |
| Windows x86-64 (MinGW)    | yes   | builds `strongtalk.exe`/`stest.exe` (PE32+); reads the image fully, then dies in the first Delta call — see [Windows](#windows) for status |

Getting the VM running end-to-end on Apple Silicon requires resolving that
remaining zone-heap walk fault in `findNMethod`. Every configuration is
verified by building from the root `Makefile`: the native arm64 config, a
**forced x86-64** config (`make ARCH=x86_64`) on macOS, the Linux/amd64
build in Docker, and the Windows x86-64 MinGW cross-build (`make OS=mingw
CXX=x86_64-w64-mingw32-g++`, in a Docker container with MinGW-w64). All four
configurations compile with zero warnings.

## Repository layout

| Path                | Contents                                          |
| ------------------- | ------------------------------------------------- |
| `vm/`               | C++ VM source                                     |
| `source/` `StrongtalkSource/` | Two snapshots of the Smalltalk library source |
| `strongtalk.bst`    | The Smalltalk image file                          |
| `test/` `easyunit/` | C++ test suite (easyunit) for the VM              |
| `build/`           | Out-of-tree per-config build dirs (`build/<arch>-<os>-<compiler>`) |
| `documentation/`    | HTML docs (typed Smalltalk, bytecodes, primitives)|
| `resources/`        | IDE resources (bitmaps, etc.)                     |

## Requirements

- A C++17 compiler: GCC 11+ or Clang
- GNU make 4+

## Building

```sh
make          # defaults to build/<arch>-<os>-<compiler>, e.g. build/arm64-macos-clang
```

Useful targets:

- `make` / `make vm` — build just the `strongtalk` VM
- `make stest` — build the test runner
- `make test` — run the C++ test suite (loads `strongtalk.bst`)
- `make docs` — regenerate `documentation/internal/vm/bytecodes.html` (runs the debug VM with `+GenerateHTML`)
- `make clean` — remove that config's build directory
- `make pristine` — like `clean`, plus that config's `.d` dependency files
- `make BUILD_DIR=/custom/path` — build into a custom directory
- `make ARCH=x86_64` — force the x86-64 backend (e.g. on an arm64 host)
- `make -j$(nproc)` — parallel build (nproc on Linux; `sysctl -n hw.ncpu` on macOS)

Cross-compiling Windows binaries (x86-64) with MinGW-w64:

```sh
# from a Linux/amd64 container that has g++-mingw-w64-x86-64 installed:
docker run --rm --platform linux/amd64 \
  -v "$PWD":/src -w /src \
  strongtalk:mingw make OS=mingw CXX=x86_64-w64-mingw32-g++ -j8
```

This writes `strongtalk.exe`, `stest.exe` and their DLLs (`strongtalk.so`,
`stest.so`, PE DLLs) into `build/x86_64-mingw-gcc/`. On Windows the shared
libraries live alongside the executables.

Building natively on Windows (x86-64) under MSYS2/MinGW-W64:

```sh
# from an MSYS2 MINGW64 shell with mingw-w64-x86-64-gcc and make installed:
make -j"$(nproc)" all
```

The MSYS2 shell exports `OS=Windows_NT`, which would fool the Makefile's
uname detection; `make` neuters the environment variable first (`ifeq ($(origin
OS),environment)` -> `OS :=`), so the build is detected as `mingw` and lands
in `build/x86_64-mingw-gcc/`.

The resulting binaries (`strongtalk`, `stest`, and their shared libraries) are
written into the per-config `build/<arch>-<os>-<compiler>/` directory. Different
configs never share objects, so switching configs needs no clean.

## Running

The VM needs the image file in the working directory:

```sh
cd build/arm64-macos-clang
DYLD_LIBRARY_PATH=. ./strongtalk   # needs strongtalk.bst at the repo root
```

On Linux use `LD_LIBRARY_PATH` instead of `DYLD_LIBRARY_PATH`.

Debug flags are toggled on the command line with `+Name`/`-Name` (for example
`+TraceBootstrap` enables the read-in trace). `TraceBootstrap` is off by
default and logs every token of the `.bst` read-in as it parses it.

`strongtalk.bst` is included at the repository root.

## Windows runtime status

The Windows x86-64 configuration **builds, links, and runs far enough to read
the image**: the full `strongtalk.bst` read-in completes (the Linux/amd64
config, by comparison, never leaves the interpreter bootstrap). Both
`strongtalk.exe` and `stest.exe` then die in the **first Delta call**
(`DeltaProcess::launch_delta`, right after the spawned Delta thread starts).
Depending on how the binaries are executed:

- **Under Wine on Apple Silicon (via Rosetta)**: the VM faults with
  `rosetta error: invalid gdt selector index 5` (SIGTRAP) — a host-emulation
  artifact of Rosetta translating the first JIT'd x86-64 code inside the Wine
  process, not a PE/runtime defect (the same generated code runs cleanly under
  Rosetta in the forced macOS x86-64 config).
- **Under native Wine on a real amd64 machine (CI)**: the VM catches an Access
  Violation in its own handler and then **hangs** — a genuine
  Windows/x86-64 port defect still to be diagnosed (needs a
  `winedbg`/backtrace capture). A run on a real Windows host would settle
  whether anything else is platform-specific.

The MSYS2 native job and the MinGW cross-build both pass their build steps in
CI; the smoke runs are best-effort (`continue-on-error`) and capped with
`timeout` so a hung process does not stall a pipeline.

## Continuous integration

- `linux.yml` — clang-format check plus the native `ubuntu-latest` (x86-64)
  build and test.
- `macos.yml` — native `macos-latest` (arm64) build.
- `windows-mingw.yml` — Windows (x86-64) cross-built on `ubuntu-latest` with
  the MinGW-w64 toolchain, smoke-tested under Wine.
- `windows-mingw-native.yml` — Windows (x86-64) built and run natively on
  `windows-latest` under MSYS2/MinGW-W64.

## Documentation

- [ARCHITECTURE.md](ARCHITECTURE.md) — the overall VM architecture: object
  system, execution engine (interpreter, compiler, inline caches), memory
  management, runtime services, and build system.
- [BYTECODES.md](BYTECODES.md) — the bytecode developer guide: formats, the
  full opcode table, inline-cache transitions, and how to add a bytecode.
- [documentation](documentation/index.html) — the original HTML documentation:
  the typed Smalltalk type-system and mixin papers, the generated bytecode
  reference, and primitive descriptions.

## License

BSD-style license. See the headers in `vm/`, plus the
[source license](sourceLicense.html) and the
[contributor licenses](contributorLicenses.html).
