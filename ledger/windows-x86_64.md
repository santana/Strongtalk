# Windows x86-64 (MinGW)

**Platform**: Windows x86-64 via MinGW-w64 (cross-built on Linux, and native MSYS2 in CI)  
**Build**: `build/x86_64-mingw-gcc`  
**Commit**: 12d3a6f + working-tree fixes (LLP64 mask widths `vm/topIncludes/bits.hpp`; `os_nt.cpp` global decl + crash diagnostics; Win64 shadow space + `call_C` arg overlap)

## Status

| Aspect | Status | Notes |
|---|---|---|
| Build | OK | Builds `strongtalk.exe`, `stest.exe`, `strongtalk.so` cleanly with `x86_64-w64-mingw32-g++`. |
| Image load | FIXED | Previously aborted with `assert(contains(q), "q must be in this space")` in `vm/memory/space.cpp:397` during heap/card-table traversal. Root cause was an LLP64 truncation bug (32-bit mask constants), now fixed; the full `strongtalk.bst` read-in completes (image-compat rewrites run, `... 0.16x secs]`). |
| First Delta call | FIXED | Was a RIP-relative disp32 wrap in `push [&last_Delta_fp]` (Root Cause 2, below); now the first Delta call runs cleanly. |
| Runtime / interpreter | BOOTS (clean exit 0) | Native amd64 wine (QEMU TCG): banner, CODE_MEM allocs, all stub routines generated, image read in ~2.9 s, the image-compat `Alien` patches execute through the real interpreter via `Delta::call_generic`, no access violation, clean exit. The `stest.exe` harness still AVs after 3 init call_generics (next open item, see "Remaining"). |
| CI | Smoke-only | CI runs Windows builds (cross/native); smoke runs are best-effort/continue-on-error and may timeout/hang in certain environments. Rosetta-translated smoke hits `rosetta error: invalid gdt selector index 5` right after image-read (translator artifact; the real-amd64 QEMU path is authoritative). |

## Diagnostics additions (working tree, os_nt.cpp)

- `extern bool bootstrapping;` fixes a latent `int` vs `bool` mismatch (the real
  global in `universe.hpp` is `bool`); the old `extern int` only compiled
  because `universe.hpp` wasn't included here. Surfaced by the new includes.
- Added a `TEMP` block to `topLevelExceptionFilter` mirroring `os_darwin.cpp`:
  when the crashing pc is in JIT code it reports the enclosing nMethod's
  selector and relocs (currently prints `no nmethod contains rip` for the
  `call_delta` stub crash — the pc is in a stub, not an nMethod).

## Root Cause 1: LLP64 mask truncation (fixed)

The early image-load abort (`object_start`, space card traversal) was a classic
LLP64 bug. `vm/topIncludes/bits.hpp` defined

```c
#define AllBits ~0UL
#define OneBit 1UL
```

On LP64 targets (macOS/Linux) `unsigned long` is 64 bits, so
`nthMask(card_shift)` produced a correct 64-bit mask. On Windows `unsigned
long` is only 32 bits, and the zero-extended 32-bit mask silently cleared the
upper 32 bits of any 64-bit address in `clearBits()`/`maskBits()`. In
`oldSpace::object_start()` this turned a valid card-aligned base

- `p = 0x00007fffbe14d988` into `q = 0x00000000be14d800`,
- tripping `assert(contains(q), "q must be in this space")`.

Fix: the constants are now `~0ULL` / `1ULL` / `0ULL` and a `static_assert`
guards the mask width against a future re-narrowing. The change is a
behavioral no-op on LP64 (values unchanged); verified by a full Windows
cross-build and a native macOS arm64 build + image load.

## Root Cause 2: first Delta call AV = unported external-word emission (x86-64)

Confirmed on **native amd64 wine** (QEMU TCG guest, Ubuntu noble, no Rosetta).
This is distinct from the shared interpreter/IC bug and Windows-specific in the
sense that Windows is the only x86-64 port and the only place the broken
encoding is exercised.

Fault site (reproducibly the very first Delta call, `Delta::call_generic`
line 101 → `StubRoutines::call_delta`):

```
rip 48 ff 35 2a d0 ee de   pushq [rip+disp32]    ; &last_Delta_fp
    48 ff 35 2b d0 ee de   pushq [rip+disp32]    ; &last_Delta_sp
    48 57                  push  rdi
    48 56                  push  rsi
    48 83 ec 20            sub   rsp,0x20
    48 89 3c 24            mov   (rsp),rdi
    48 89 74 24 08         mov   8(rsp),rsi
    ...
Access violation: reading at <unmapped page>
```

The AV is **reading the global variable in the push**: `assembler_x86.cpp:376`
emits `emit_long((int)(disp - next_pc))` i.e. the 64-bit absolute address minus
the next pc truncated to a signed 32-bit RIP-relative disp32. In the QEMU/wine
layout `strongtalk.so` (with `last_Delta_fp` etc. in `.bss`) loads ~0x40_1A00_0000
(~17 GB) below the VM's JIT code arena, so the wrapped disp points ~17 GB off
into an unmapped page → AV before the first Delta method even runs.

`set_last_Delta_frame_before_call/reset_last_Delta_frame`
(`assembler_x86.cpp:1774-1792`) and every other
`Address((intptr_t)&c_global, relocInfo::external_word_type)` site on x86-64
share the defect; the comment there ("must be fixed up to RIP-relative addresses
by the relocation machinery (64-bit port item)") marks it as unfinished port
work. AArch64 uses `load_absolute_address` (a register-indirect 64-bit load),
which is why the macOS/arm64 build reaches the same stage without faulting.

Designed fix (not yet implemented): make `Address(&c_global, external_word)`
materialize the 64-bit address in a scratch register (e.g. `movabs` + indirect
load/store/push/pop), or place the code arena within signed-32-bit reach of the
image data (fragile under ASLR). A table-of-globals near the code arena is the
robust option.

## Root Cause 3 (FIXED): missing Win64 C-call shadow space corrupted the delta stack

The interpreter/IC "receiver decode" symptom (Root Cause 3 below, and the long
`0x7fffb74f1b99` execute-PF series) was **not a genuine decode bug** — it was the
missing Windows x64 C-call contract. Every engine->C call emitted by
`X86MacroAssembler::call_C*` did `set_last_Delta_frame_before_call(); call(...);
reset_last_Delta_frame()` with **no 32-byte shadow space and no 16-byte stack
alignment**. MingW C callees spill their register args into the caller's stack
area *above* the return address — precisely the interpreter's live delta-stack
words (`[method oop][bytecodes][receiver]`). So on the first real C call through
the interpreter (e.g. `InterpretedIC::inline_cache_miss`) the C function
stamped the receiver/IC slots; the "receiver" that `frame::current_interpretedIC()`
then read was garbage (often the string oop `0x7fffb74f1b99`), the ic-miss kept
re-entering do-it with that bogus receiver, and the interpreter finally *executed*
the string oop → execute-PF at `0x7fffb74f1b99` (all prior invariants, incl. the
`rdx=0` state, were downstream artifacts of this).

Fix (`vm/asm/assembler_x86.cpp` + `vm/asm/interpreterBackend_x86.cpp`):

- New `X86MacroAssembler::win64_call_shadow_begin/end`: stash `esp` in `edi`
  (dead across engine->C calls), `and esp,-16`, `sub esp,32`, call, restore
  `esp` from the stash. No-op outside `_WIN64`. Applied to every C-crossing
  `call_C` overload (all `char*`/`Register` entry forms and the 1-4 arg forms)
  plus the stack-arg callers (`call_trace_DLL_call_1`, pascal callback stub),
  `callPopStackHandles`, and `callScavengeAndAllocate`.
- The shadow area now sits *below* the live delta-stack words, and `esp` is
  16-byte aligned at every C call.
- Fixed Win64 register-passing SysV-isms: `callScavengeAndAllocate`
  used `edi` for arg1 on all targets (now `rcx` on Win64); the `Register`
  overload now goes through `call_C` (ABI select + shadow).
- Fixed 3-/4-arg `call_C` Win64 arg overlap: an argument that *itself* lives in
  a Win64 argument register (e.g. `call_C(fn, eax, ebx, edx, ecx)` from
  `call_unpack_unoptimized_frames`/`verifyNLR`) was overwritten by an earlier
  dest move (this tripped the old assert at boot). Such args are staged in
  `r11`/`r10` before the dest moves.

Result: `strongtalk.exe -b strongtalk.bst` boots to a clean exit-0 on native
amd64 wine (QEMU TCG), executing real Delta sends (`Delta::call_generic`) for
the image-compat patches with no access violation.

## Root Cause 3 (orig.) - RESOLVED

See "Root Cause 3 (FIXED)" above: the SMI-tagged-receiver symptom was the shadow-space
corruption, not an independent decode bug.

## Root Cause 4 (known, originally reported as the string-execute PF)

The `0x7fffb74f1b99` execute-PF was the observable of Root Cause 3 (FIXED).

## Remaining (post-boot)

- `stest.exe` exits 5 (wine NTSTATUS `0xC0000005` low byte) after 3 init
  `call_generic`s — the easyunit harness path needs debugging (next item).
- The doesNotUnderstand tail of `generate_inline_cache_miss_handler`
  (`interpreter.cpp`) still pops args with a 32-bit `addl(esp, ecx)` (bytes, not
  `argCount*oopSize`) and 32-bit `movl`s — dormant path, needs an x64 review.
- DLL-call stubs and the *compiler* C-call sites
  (`codeGenerator.cpp:1614`, `oldCodeGenerator.cpp:987`) do not yet reserve
  shadow space — needed for external DLL calls and compiled code.
- Rosetta-translated smokes can fault on `rosetta error: invalid gdt selector
  index 5` (translator limitation) and are not usable as a gate for this region;
  the native amd64 QEMU TCG + wine path is authoritative.

## Evidence

- Cross-build succeeds in Docker `strongtalk:mingw-wine`.
- Before the fix: smoke run aborts with `assert(contains(q), ...)` right after
  `[Reading in strongtalk.bst, n`.
- After the fix, smoke run output (Wine/Rosetta): image-compat patches run,
  `, 0.166 secs]` read-in completes, then the emulated first-Delta-call fault.
- Native amd64 (QEMU TCG + wine 9.0, Ubuntu noble): VM handler report +
  winedbg register dump for the `call_delta` push-AV; return chain symbolized
  with `x86_64-w64-mingw32-addr2line` (ImageBase 0x293720000) → `Delta::call_generic`
  at `delta.cpp:101`. The pushed targets compute to `&last_Delta_fp`/`&last_Delta_sp`
  (`.bss` RIP offsets `0x61b348`/`0x61b350` from the module base), confirming the
  disp32-wrap.
- Artifacts exist: `strongtalk.exe`, `stest.exe` in `build/x86_64-mingw-gcc/`.
- Final boot evidence (native amd64 QEMU TCG + wine, `setarch x86_64 -R`):
  banner → CODE_MEM allocs → all stub routines generated (incl. the previously
  asserting `unpack_unoptimized_frames`) → `[Reading in strongtalk.bst, ...
  Alien>>ensureLoaded ... , 2.9 secs]` → clean `exit=0`, no access violation.
  Pre-fix the same run ended in the execute-PF at `0x7fffb74f1b99`.

## Reproduction

Cross-build smoke (Wine-on-Apple-Silicon, the fast path):

```sh
docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src strongtalk:mingw-wine bash -c "
  make -j$(nproc) OS=mingw CXX=x86_64-w64-mingw32-g++ >/dev/null 2>&1 &&
  cd build/x86_64-mingw-gcc &&
  cp \$(dirname \$(x86_64-w64-mingw32-g++ -print-file-name=libgcc_s_seh-1.dll))/*.dll . 2>/dev/null &&
  cp /usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll . 2>/dev/null &&
  WINEDEBUG=-all timeout 120 xvfb-run -a /usr/lib/wine/wine64 ./strongtalk.exe -b ../../strongtalk.bst
"
```

Native amd64 repro (real AV + winedbg, no Rosetta). Boot Ubuntu noble amd64 in
QEMU TCG with the Windows build on a vvfat drive and a hostfwd'd ssh port, then:

```sh
# in the guest, as root:
modprobe vfat; mkdir -p /mnt /root/build
mount -o ro /dev/vdb1 /mnt && cp -r /mnt/* /root/build/.
cd /root/build
WINEDEBUG=-all timeout 120 /usr/lib/wine/wine64 ./strongtalk.exe -b strongtalk.bst   # VM AV report
printf 'c\nbt 120\ninfo registers\nquit\n' | timeout 150 winedbg ./strongtalk.exe -b strongtalk.bst
```

Symbolize `strongtalk.so` frames (PE ImageBase 0x293720000, DWARF-5):

```sh
x86_64-w64-mingw32-addr2line -e build/x86_64-mingw-gcc/strongtalk.so -f -C \
  0x293827a71   # Delta::call_generic (delta.cpp:101)
```