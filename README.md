# Strongtalk

[![Build status](https://github.com/santana/Strongtalk/actions/workflows/build.yml/badge.svg)](https://github.com/santana/Strongtalk/actions/workflows/build.yml)

An optionally-typed Smalltalk with a high-performance optimizing JIT, developed
by LongView Technologies LLC (1994-1997) and open-sourced by Sun Microsystems in
2006. The VM is written in C++ and self-hosts its own optimizing compiler and
garbage collector.

## Status

The VM is a work in progress as a project. The C++ code builds on **Linux
(x86-64)**, **macOS (Apple Silicon)**, and **Windows (x86-64, MinGW
cross-build)** via the portable root `Makefile` (out-of-tree builds), and a CI
build runs on every push.

The JIT/code generator has a **single frontend with per-architecture
backends**: it emits **x86-64 machine code** on x86-64 and **AArch64 machine
code** on Apple Silicon. The AArch64 backend is ported and exercising the full
JIT pipeline (compiler, scope-description recording, inline caches, jumps,
deoptimization, and recompilation): the VM boots, the image read-in completes
fully, JIT-compiled frames install and run, and recompile/deopt cycles execute.
It currently aborts during a hot-method recompile on
`assert(methodHeap->contains(n), "not in zone")` in `zone::findNMethod`
(`zone.cpp:622`), the active blocker.

| Platform                  | Build | Runtime                                                          |
| ------------------------- | ----- | ---------------------------------------------------------------- |
| Linux x86-64 (native)     | yes   | loads the image, then spins in the interpreter bootstrap loop (repeated `error:` re-raise in `runBaseClassInitializers`); no JIT code yet |
| macOS arm64 (AArch64)     | yes   | boots, loads the image, runs JIT-compiled code; blocked at a `findNMethod` "not in zone" assert (`zone.cpp:622`) during recompile |
| Windows x86-64 (MinGW cross-build) | yes | builds `strongtalk.exe`/`stest.exe` (PE32+); runtime not yet exercised (needs Wine or Windows) |
| Windows (legacy)          | yes   | via `build.win32` (Visual Studio, x86 only)                       |

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
| `build.win32/`      | Visual Studio project (Windows)                   |
| `bin/`              | Legacy Windows build scripts and prebuilt objects |
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

## Continuous integration

`.github/workflows/build.yml` builds on `ubuntu-latest` (x86-64),
`macos-latest` (arm64), and — on `ubuntu-latest` with the MinGW-w64 cross
toolchain — Windows (x86-64) for every push and pull request. A native
`windows-latest` job also builds and runs the Windows binaries with
MSYS2/MinGW-W64.

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
