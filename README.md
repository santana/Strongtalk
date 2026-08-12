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

The JIT/code generator emits **x86-64 machine code only**:

| Platform                      | Build | Runtime                                                          |
| ----------------------------- | ----- | ---------------------------------------------------------------- |
| Linux x86-64 (native)         | yes   | boots, loads the image, reaches interpreter codegen; stops at a VM assert (`interpreter.cpp:279`) |
| macOS arm64                   | yes   | blocked: the x86-64 codegen cannot execute on Apple Silicon      |
| Windows                       | yes   | via `build.win32` (Visual Studio, x86 only)                       |

Getting the VM running end-to-end on any of these platforms requires fixing the
remaining runtime asserts, and (for Apple Silicon) porting the code generator to
AArch64.

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

`.github/workflows/build-unix.yml` builds `build.unix` on `ubuntu-latest` and
`macos-latest` for every push and pull request, and uploads the binaries as
build artifacts.

## Documentation

See the [documentation](documentation/index.html) for the original HTML
documentation, including the typed Smalltalk type-system and mixin papers, the
bytecode reference, and primitive descriptions.

## License

BSD-style license. See the headers in `vm/`, plus the
[source license](sourceLicense.html) and the
[contributor licenses](contributorLicenses.html).
