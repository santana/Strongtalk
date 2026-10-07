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
code** on Apple Silicon. The same frontend/backend split extends to the
**interpreter**: bytecode handlers are emitted `#ifdef`-free through a
per-arch `InterpreterBackend` layer that centralizes the delta-stack slot
model, primitive-call/return ABI, NLR and method-return conventions, and
megamorphic lookup-cache probing (see ARCHITECTURE.md §2.5.1), with the
per-arch machine output gated to stay byte-identical. The AArch64 backend is
ported and exercising the full JIT pipeline (compiler, scope-description
recording, inline caches, jumps, deoptimization, and recompilation): the VM
boots, the image read-in completes fully, JIT-compiled frames install and run,
and recompile/deopt cycles execute. The earlier `findNMethod` "not in zone"
assert (`zone.cpp:622`, once the active blocker) is resolved, as is the frozen
image's 4-byte `Alien` in `Alien>>ensureLoaded:` — `vm/runtime/imageCompat.*`
rewrites that site at image load (`[image-compat] Alien>>ensureLoaded: alien
size 4 -> 8 bytes`), so `libobjc.dylib` now loads and DLL handles resolve
normally.

**Both 64-bit backends now run real Smalltalk.** A long-standing defect made
`frame::is_interpreted_activation()` validate a memOop's **mark** word as if it
were its **klass**; the mark normally holds the non-canonical
`markOopDesc::tagged_prototype()` sentinel, so the check rejected every hybrid
code pointer, inline-cache misses could not find their cache, and **every send
in every interpreted method became a silent no-op**. Fixed 2026-10-01
(`frame::method_from_hp()` now validates word 1 via `klass_addr()`). Before
the fix the scheduler's `ProcessorScheduler>>start` ran its seven bytecodes,
dropped all four sends and returned; after it, the system image initialises and
the image's own error-handling machinery executes on both backends.

| Platform                  | Build  | Runtime                                                          |
| ------------------------- | ----- | ---------------------------------------------------------------- |
| Linux x86-64 (native)     | yes   | boots, loads the image, and runs the interpreter into the image's error handler (`1-ProcessExplicitError` on `2-ProcessorScheduler`). Then `Unhandled error in the scheduler` re-raises in a loop (~300k register dumps in 45 s); never reaches `Eval>` |
| macOS arm64 (AArch64)     | yes   | boots, loads the image, passes GC, resolves DLL handles and `LoadImageA`. Stopped on the post-boot DLL-loading path by `klass 0x1 isn't a klass`, a clean `LookupKey::verify()` diagnostic whose underlying cause is an SMI-tagged word in a send's receiver slot (AArch64 **A8**) |
| macOS x86-64 (forced)     | yes   | boots, runs the image's error handler (`#2 Error defaultAction`, `#3 BlockExceptionHandler block`) to completion, and **reaches the `Eval>` prompt**. Stopped by the same `klass 0x1 isn't a klass` diagnostic, same SMI-receiver root cause (x86-64 **X15**) |
| Windows x86-64 (MinGW)    | yes   | builds `strongtalk.exe`/`stest.exe` (PE32+); reads the image fully, then dies in the first Delta call — see [Windows](#windows-runtime-status) for status |

macOS x86-64 now reaches `Eval>`; the others do not. The remaining blocker is
shared by both macOS arches and is in the VM's own send path, not in the image
and not in the code generator: the interpreter's receiver-slot decode for sends
hands `InterpretedIC::inline_cache_miss()` a non-pointer (`0xc`,
`0x7ffffffffffffffc` — both SMI-tagged) where the receiver object should be, so
`receiver->klass()` reads ordinary memory through a bogus base. See [ledger/](ledger/) for detailed per-architecture status and reproduction notes.

Getting the VM running end-to-end requires fixing that receiver-slot decode.
Every configuration is verified by building from the
root `Makefile`: the native arm64 config, a
**forced x86-64** config (`make ARCH=x86_64`) on macOS, the Linux/amd64
build in Docker, and the Windows x86-64 MinGW cross-build (`make OS=mingw
CXX=x86_64-w64-mingw32-g++`, in a Docker container with MinGW-w64). All four
configurations compile with zero errors. The inherited MinGW `smiOop.hpp`
`long`-width shift toxics were fixed (2026-09-24, `INT64_C(1)` plus a wider
local in `debug_prims.cpp`); the warnings that remain are all pre-existing and
outside current changes — `-Wundefined-inline` reports from
`oop.hpp`/`generation.hpp` on the POSIX configs, plus on MinGW the same six
and `proxyOop.cpp` (`-Wint-to-pointer-cast`) and `proxy_prims.cpp`
(`-Wshift-count-overflow`).

## Repository layout

| Path                | Contents                                          |
| ------------------- | ------------------------------------------------- |
| `vm/`               | C++ VM source                                     |
| `source/`            | Legacy Smalltalk library source dump (fileout format with extensionless files and .class stubs; contains change/save logs) |
| `StrongtalkSource/`  | Primary Smalltalk library source (Delta `.dlt`/`.str`/`.gr` chunk files) |
| `strongtalk.bst`    | The Smalltalk image file                          |
| `test/` `easyunit/` | C++ test suite (easyunit) for the VM              |
| `build/`           | Out-of-tree per-config build dirs (`build/<arch>-<os>-<compiler>`) |
| `documentation/`    | HTML docs (typed Smalltalk, bytecodes, primitives)|
| `resources/`        | IDE resources (bitmaps, etc.)                     |

## Requirements

- A C++17 compiler: GCC 11+ or Clang
- GNU make 4+
- clang-format (>= 18; pinned to 23.1.0 so local formatting matches CI) —
  install with `make setup-deps`

## Building

```sh
make          # defaults to build/<arch>-<os>-<compiler>, e.g. build/arm64-macos-clang
```

Build output is terse: one short line per step (`CXX vm/...`, `LINK strongtalk`).
Use `make V=1` to print the full command lines instead, or `make -s` for silence.

Useful targets:

- `make` / `make vm` — build just the `strongtalk` VM
- `make stest` — build the test runner
- `make test` — run the C++ test suite (loads `strongtalk.bst`)
- `make docs` — regenerate `documentation/internal/vm/bytecodes.html` (runs the debug VM with `+GenerateHTML`)
- `make clean` — remove that config's build directory
- `make pristine` — like `clean`, plus that config's `.d` dependency files
- `make format-check` — verify sources obey `.clang-format` (CI gate)
- `make version` — print the version/commit/date/config this build would embed
- `make package` — build a self-contained, runnable, versioned archive (see [Packaging](#packaging))
- `make setup-deps` — install the dev tools (clang-format pinned to CI's version)
- `make install-hooks` — enable the pre-commit hook that runs `make format` on staged files
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
# from an MSYS2 MINGW64 shell with mingw-w64-x86_64-gcc and make installed:
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

## Version reporting

Every build stamps its identity into the binary, so it can be traced back to
the exact source it came from:

```sh
$ ./strongtalk -version          # also: --version
Strongtalk 0.0.0-dev (v0.9.0-42-g9f3a7b1-dirty, 2026-09-30T11:00:56Z, arm64-macos-clang)
```

The parts are:

| Part | Source |
| --- | --- |
| version | the nearest `git tag`, `v` stripped (`0.0.0-dev` before the first tag) |
| describe | `git describe --tags --always --dirty` — `-N-gHASH` when ahead of a tag, `-dirty` with uncommitted changes |
| date | build timestamp, ISO-8601 UTC |
| config | the `<arch>-<os>-<compiler>` build directory triple |

`make version` prints the same line without building. The build system reads
git directly, so a build from a source tarball — or from a container image
without a `git` binary, such as `strongtalk:linux-tools` — reports only the
parts it can determine (the hash/describe is then omitted rather than faked).
A committed `VERSION` file in the repository root supplies the version in that
case; anything can be overridden explicitly:

```sh
make SEMVER=1.4.0 GIT_DESC=v1.4.0 GIT_HASH=abc1234   # pin the identity
make BUILD_DATE=2026-01-01T00:00:00Z                 # reproducible builds
```

Debug flags are toggled on the command line with `+Name`/`-Name` (for example
`+TraceBootstrap` enables the read-in trace). `TraceBootstrap` is off by
default and logs every token of the `.bst` read-in as it parses it.

`strongtalk.bst` is included at the repository root.

## Packaging

`make package` collects the build into a single archive that is meant to be
unpacked and run as-is:

```sh
make package
#   PKG     strongtalk-0.0.0-dev-719abcf-arm64-macos-clang.tar.gz
```

It lands in the config's build directory and contains:

| Entry | Purpose |
| --- | --- |
| `strongtalk`, `stest` | the executables |
| `strongtalk.so`, `stest.so` | the VM/test shared libraries (PE DLLs on Windows) |
| `strongtalk.bst` | the image the VM reads — without it the binaries cannot start |
| `run.sh` (Unix) / `run.bat` (Windows) | launcher that sets the loader path / working directory |
| `VERSION.txt` | the full build identity, the same string `-version` reports |
| `README.txt` | the same, tailored to the platform |
| `libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll` | Windows only: the MinGW runtime the VM DLL imports |

The archive name embeds the version, the commit and the build config, so a
downloaded file identifies itself:

```
strongtalk-<version>-<commit>-<config>.tar.gz     # .zip on Windows
strongtalk-1.4.0-a1b2c3d-x86_64-linux-gcc.zip
```

The build date is deliberately **not** in the name: it differs on every run,
which would both mint a new name per build and make the archive
non-reproducible. `make version-file-tag` prints just that name component, which
is how CI names the artifact without ever drifting from the Makefile.

Knobs:

- `make package PACKAGE_FORMAT=tar.gz` — force a tarball even on Windows
- `make package MINGW_RUNTIME_DLLS=` — skip the MinGW DLLs
- `PACKAGE_FORMAT=zip` needs one of `zip`, `bsdtar` or `powershell.exe` (the
  last is normally present on MSYS2, where `zip` is not)

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

These four run on every push and pull request and upload **nothing**: they are
the correctness gate. Binaries are published only for tags, by

- `release.yml` — on a `v*` tag push, builds all four configurations, runs
  `make package` in each, and attaches the four versioned bundles to a GitHub
  Release. Each bundle is uploaded as a workflow artifact named
  `strongtalk-<platform>-<version>-<commit>-<config>`.

Tag pushes need a full clone: the semantic version comes from
`git describe --tags`, so the checkout uses `fetch-depth: 0`.

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
