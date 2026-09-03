# Strongtalk

[![Build status](https://github.com/santana/Strongtalk/actions/workflows/build-unix.yml/badge.svg)](https://github.com/santana/Strongtalk/actions/workflows/build-unix.yml)

An optionally-typed Smalltalk with a high-performance optimizing JIT, developed
by LongView Technologies LLC (1994-1997) and open-sourced by Sun Microsystems in
2006. The VM is written in C++ and self-hosts its own optimizing compiler and
garbage collector.

## Status

The VM is a work in progress as a project. The C++ code builds on **Linux
(x86-64)** and **macOS (Apple Silicon)** via the portable `build.unix` makefiles,
and a CI build runs on every push.

The JIT/code generator has a **single frontend with per-architecture
backends**: it emits **x86-64 machine code** on x86-64 and **AArch64 machine
code** on Apple Silicon. The AArch64 backend is ported and exercising the full
JIT pipeline (compiler, scope-description recording, inline caches, and jumps) —
enough that the VM boots, loads the image, and installs JIT-compiled frames on
arm64. It currently stops at an indirect-call fault inside JIT dispatch
(`blr x16` to a tagged oop), the active blocker.

| Platform                  | Build | Runtime                                                          |
| ------------------------- | ----- | ---------------------------------------------------------------- |
| Linux x86-64 (native)     | yes   | boots, loads the image, runs JIT-compiled code                   |
| macOS arm64 (AArch64)     | yes   | boots, loads the image, installs JIT frames; blocked at an indirect-call (`blr x16`) fault during dispatch |
| Windows                   | yes   | via `build.win32` (Visual Studio, x86 only)                       |

Getting the VM running end-to-end on Apple Silicon requires resolving that
remaining dispatch fault. The portable `build.unix` tree is verified by building
**both** the native arm64 configuration and a **forced x86-64** configuration
(`make ARCH_FLAGS=-arch x86_64`).

## Repository layout

| Path                | Contents                                          |
| ------------------- | ------------------------------------------------- |
| `vm/`               | C++ VM source                                     |
| `source/` `StrongtalkSource/` | Two snapshots of the Smalltalk library source |
| `strongtalk.bst`    | The Smalltalk image file                          |
| `test/` `easyunit/` | C++ test suite (easyunit) for the VM              |
| `build.unix/`       | Portable makefiles (Linux + macOS)                |
| `build.win32/`      | Visual Studio project (Windows)                   |
| `bin/`              | Legacy Windows build scripts and prebuilt objects |
| `documentation/`    | HTML docs (typed Smalltalk, bytecodes, primitives)|
| `resources/`        | IDE resources (bitmaps, etc.)                     |

## Requirements

- A C++17 compiler: GCC 11+ or Clang
- GNU make 4+

## Building

```sh
cd build.unix
make all          # builds strongtalk (VM) and stest (test runner)
```

Useful targets:

- `make` / `make vm` — build just the `strongtalk` VM
- `make stest` — build the test runner
- `make test` — run the C++ test suite (loads `strongtalk.bst`)
- `make clean` — remove objects and binaries
- `make -j$(nproc) all` — parallel build (nproc on Linux; `sysctl -n hw.ncpu` on macOS)

The resulting binaries (`strongtalk`, `stest`, and their shared libraries) are
written into `build.unix/`.

## Running

The VM needs the image file in the working directory:

```sh
cd build.unix
./strongtalk            # needs strongtalk.bst and optionally a source/ directory
```

`strongtalk.bst` is included at the repository root.

## Continuous integration

`.github/workflows/build-unix.yml` builds on `ubuntu-latest` (x86-64) and
`macos-latest` (arm64) for every push and pull request.

## Documentation

See the [documentation](documentation/index.html) for the original HTML
documentation, including the typed Smalltalk type-system and mixin papers, the
bytecode reference, and primitive descriptions.

## License

BSD-style license. See the headers in `vm/`, plus the
[source license](sourceLicense.html) and the
[contributor licenses](contributorLicenses.html).
