# Windows x86-64 (MinGW)

**Platform**: Windows x86-64 via MinGW-w64 (cross-built on Linux, and native MSYS2 in CI)  
**Build**: `build/x86_64-mingw-gcc`  
**Commit**: 144eed2

## Status

| Aspect | Status | Notes |
|---|---|---|
| Build | OK | Builds `strongtalk.exe`, `stest.exe`, `strongtalk.so` cleanly with `x86_64-w64-mingw32-g++`. |
| Image load | PARTIAL | Reads `strongtalk.bst` header/initialization; image load proceeds until hitting an assertion during space/card table processing (on Wine/amd64 host emulation in the smoke test). |
| Runtime | FAILS EARLY | In Wine smoke test, VM aborts with `assert(contains(q), "q must be in this space")` in `vm/memory/space.cpp:397` (oldSpace::object_start). Also reports emulation artifacts under Rosetta/Wine depending on environment. |
| CI | Smoke-only | CI runs Windows builds (cross/native) but smoke runs are best-effort/continue-on-error and may timeout/hang in certain environments. |

## Current Failure

From the Wine smoke capture:

- Banner and image load messages appear; image compat rewrites run.
- Then: `[A Runtime Error Occurred] assert(contains(q), "q must be in this space") /src/vm/memory/space.cpp, 397`
- Followed by VM event log and host emulation noise (`rosetta error: invalid gdt selector index 5` when run under Rosetta on Apple Silicon, though Wine environment specifics vary).

The failing assertion is in `oldSpace::object_start(oop* p)`: after computing `q` from card table, it does `assert(contains(q), "q must be in this space")`. This occurs when `object_start` is called on an address whose card-table derived base is not contained in the old space. This can happen if a slot value is bogus (non-pointer) or if the space/card table is in an inconsistent state during image load/initialization — consistent with issues around how untrusted addresses are handled (see also `object_start_checked` used in frame/method-from-hp paths).

## Evidence

- Cross-build succeeds in Docker `strongtalk:mingw-wine`.
- Smoke run under Wine/xvfb produces the assert in `space.cpp:397` early after image load.
- Artifacts exist: `strongtalk.exe`, `stest.exe` in `build/x86_64-mingw-gcc/`.

## Known Issues / References

- `ARCHITECTURE.md` §6 / README Windows section: Windows x86-64 builds and reads image fully, then dies in first Delta call (behavior described varies by environment — Wine/Rosetta artifacts noted). The captured assert is an earlier hard failure in `object_start` during space traversal.
- `vm/memory/space.cpp:391-414` (`oldSpace::object_start`): trusts card table; a bogus pointer can make `q` point outside the space. `Universe::object_start_checked()` exists for untrusted addresses — any path calling `object_start()` on untrusted values during early boot needs to be hardened.
- Image load involves many heap traversals; the assert suggests a pointer derived from the image or from intermediate state is being misinterpreted.

## Reproduction

```sh
docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src strongtalk:mingw-wine bash -c "
  make -j$(nproc) OS=mingw CXX=x86_64-w64-mingw32-g++ >/dev/null 2>&1 &&
  cd build/x86_64-mingw-gcc &&
  cp \$(dirname \$(x86_64-w64-mingw32-g++ -print-file-name=libgcc_s_seh-1.dll))/*.dll . 2>/dev/null &&
  cp /usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll . 2>/dev/null &&
  WINEDEBUG=-all timeout 120 xvfb-run -a /usr/lib/wine/wine64 ./strongtalk.exe -b ../../strongtalk.bst
"
```
