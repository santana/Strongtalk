# VM Overflow Hardening Ledger (commit 144eed2)

This ledger tracks the buffer/overflow audit findings and their remediation.
All changes are read-only-audit driven; trusted-image execution is in scope, malicious snapshots are not.

## Legend
- **Sev:** Critical / High / Med-High / Medium / Low–Med / Low
- **Status:** Open | In Progress | Fixed | Deferred | Refuted
- **Verification:** Build | Tests (baseline) | Manual | ASan

---

## F-001 (Critical) — objArray length truncation → GC wild traversal

- **Files:** `vm/prims/objArray_prims.cpp:55,63,77,89,94,103`, `vm/oops/objArrayKlass.cpp:149,162,171`
- **Sev:** Critical
- **Trigger:** `VM new: Array size: 2^32` (or any smi with `value != (int)value`), then GC (`scavengeGarbage`/`collectGarbage`).
- **Root cause:** Primitives narrow 64-bit smi `value` to `int obj_size` but store raw `argument` (full smi) into length slot. Traversal loops use `base + o->length()` (untruncated) while allocated size is `object_size(truncated)`.
- **Proposed fix:** 
  1. In `objArrayPrimitives::allocateSize` and `allocateSize2`, check `intptr_t v = smiOop(argument)->value();` reject if `v < 0` or `v != (int)v` (or cap to reasonable max). Return error symbol (e.g. `vmSymbols::negative_size()` or `vmSymbols::out_of_memory()`/add `bad_size` if needed). 
  2. Defensive: bound traversal by allocated size (`min(length(), object_size(o))`) in `objArrayKlass` scavenge/follow loops.
- **Status:** Fixed (objArray primitives + traversal)
- **Verification:** Build succeeds (arm64-macos-clang). No behavior change for valid sizes.

---

## F-002 (High) — weakArray off-by-one in mark-sweep follow

- **File:** `vm/oops/weakArrayKlass.cpp:107-118` (follow), also `:86,101` (scavenge)
- **Sev:** High
- **Trigger:** WeakArray reachable only via `NotificationQueue` post-registration scan (`markSweep.cpp:266,269,275,276`).
- **Root cause:** `while (base <= end)` visits one word past payload; `reverse_and_follow` can write/follow OOB.
- **Proposed fix:** Change `<=` to `<` in `oop_follow_contents`. Optionally fix scavenge loops for consistency (currently dead).
- **Status:** Fixed (scavenge + follow loops)
- **Verification:** Build succeeds (arm64-macos-clang). No behavior change for valid arrays.

---

## F-003 (High) — Unbounded `%[a-zA-Z]` in CLI token parsing

- **File:** `vm/runtime/arguments.cpp:59-61`
- **Sev:** High
- **Trigger:** Unrecognized non-`-`/`+` argv token with ≥100 leading letters, or equivalent token in settings file; overflow before `sscanf` returns.
- **Root cause:** `%[a-zA-Z]` has no field width into `char name[100]`.
- **Proposed fix:** Use width `%99[a-zA-Z]` (or `sizeof(name)-1`) and handle truncation. Also guard by token length.
- **Status:** Fixed
- **Verification:** Build succeeds.

---

## F-004 (Med-High) — Settings file token accumulator unbounded

- **File:** `vm/runtime/arguments.cpp:76,91-109`
- **Sev:** Med-High
- **Trigger:** ≥1024-byte whitespace-free run in `~/.strongtalkrc` or `-f <file>`.
- **Root cause:** `pos` never checked against `sizeof(token)-1`.
- **Proposed fix:** Check `pos < sizeof(token)-1` before `token[pos++] = c` in both branches; on overflow, null-terminate at boundary and process token (or break), then continue.
- **Status:** Fixed
- **Verification:** Build succeeds.

---

## F-005 (Medium) — byte/double-byte array: missing NULL check after allocate

- **Files:** `vm/prims/byteArray_prims.cpp:55-69`, `vm/prims/dByteArray_prims.cpp:55-62`
- **Sev:** Medium
- **Trigger:** Large allocation whose truncated size exceeds reservation → `Universe::allocate` returns NULL, dereferenced before check.
- **Root cause:** No NULL check before `initialize_header`/`set_length`.
- **Proposed fix:** Check for NULL; return appropriate error symbol (e.g. `vmSymbols::out_of_memory()`). Also added length truncation guards.
- **Status:** Fixed (byteArray + dByteArray)
- **Verification:** Build succeeds.

---

## F-006 (Medium) — `oldSpace::expand_and_allocate` int×int overflow

- **Files:** `vm/memory/space.cpp:133,268`
- **Sev:** Medium
- **Trigger:** Request > ~2^28 oops reaches `expand(size * oopSize)`.
- **Root cause:** Signed `int` multiplication `size * oopSize`.
- **Proposed fix:** Compute in 64-bit, clamp or fail safely; change arithmetic to avoid signed overflow.
- **Status:** Open
- **Verification:** Build; no change for normal sizes.

---

## F-007 (Medium) — Linux dlerror: unbounded sprintf + format-string warning

- **File:** `vm/runtime/os_linux.cpp:365-372`
- **Sev:** Medium
- **Trigger:** `dlopen`/`dlsym` failure with very long name (name bounded to ~199 by `dll.cpp`).
- **Root cause:** `sprintf` into `malloc(200)`; `warning(message)` passes runtime string as format.
- **Proposed fix:** Use `snprintf(message, 200, format, dlerror());` and `warning("%s", message);`.
- **Status:** Fixed (Linux paths)
- **Verification:** Build succeeds.

---

## F-008 (Medium latent) — JIT stack-init units mismatch + emit-before-validate

- **Files:** `vm/compiler/codeGenerator.cpp:377-394,931-943`, `vm/asm/assembler.cpp:41-52,54-60`, `vm/asm/codeBuffer.cpp:50-60`
- **Sev:** Medium (latent correctness)
- **Trigger:** Method with many stack temporaries (e.g. >5 on x86-64) can overrun patch buffer before fatal.
- **Root cause:** Guard compares push-count `n` vs byte capacity; emit writes before bounds check.
- **Proposed fix:** Fix units (byte-based guard for actual emit sizes: x86 push 2B, AArch64 push 4B). Add pre-check. Keep structural change minimal.
- **Status:** Deferred (complex, needs backend-specific sizing; low practical risk for ordinary code; documented)
- **Verification:** Pending.

---

## F-009 (Low–Med) — Debug evaluator unbounded reads/concats

- **File:** `vm/runtime/evaluator.cpp:115-120,321-333,402,245-247,255-257`
- **Sev:** Low–Med
- **Trigger:** Long input at `Eval>` prompt (debug REPL).
- **Root cause:** `get_line` unbounded; `strcat` into `name[100]`, overflow of `arguments[10]`; unbounded `%[...]` in predicates.
- **Proposed fix:** Add bounds checks, cap `nofArgs < 10`, use width in `%[...]` (e.g. `%39[a-zA-Z]`).
- **Status:** Fixed
- **Verification:** Build succeeds; bounds added.

---

## F-010 (Low) — error.cpp strcat unchecked

- **File:** `vm/memory/error.cpp:66-82`
- **Sev:** Low
- **Trigger:** `+ShowMessageBoxOnError` and formatted error text > 2012 bytes.
- **Root cause:** `strcat` after `vsnprintf` without checking remaining space.
- **Proposed fix:** Check remaining bytes before append; truncate if needed, or check `vsnprintf` return.
- **Status:** Fixed
- **Verification:** Build; no behavior change in common cases.

---

## F-011 (Low) — warning called with runtime string as format

- **Files:** `os_linux.cpp:372`, `os_darwin.cpp:407`
- **Sev:** Low
- **Trigger:** `dlerror()` contains `%` character.
- **Root cause:** Missing format string, passes message as format.
- **Proposed fix:** `warning("%s", message);`
- **Status:** Fixed
- **Verification:** Build; output unchanged.

---

## F-012 (Low robustness) — signed int overflow in space size scaling

- **File:** `vm/memory/spaceSize.cpp:31-33`
- **Sev:** Low (robustness)
- **Trigger:** Large flag values (e.g. `ReservedHeapSize=3000000`) in settings.
- **Root cause:** `value * K` uses signed `int`.
- **Proposed fix:** Compute in `int64_t`, clamp to reasonable bounds or fail safely.
- **Status:** Deferred (fails loudly today)
- **Verification:** Pending.

---

## Deferred / Refuted (for record)
- objArray print uses `min(MaxElementPrintSize,len)` — **Refuted** (no over-read).  
- `rSet` multi-byte size spill — **Refuted** (correct given encoding).  
- `os_linux.cpp:363`, `os_darwin.cpp:398` `strcpy` into exactly-sized `malloc` — **Refuted**.  
- env-var parsing `#ifdef unused_but_maybe_useful_later` — **Refuted** (dead).  
- JIT width divergence (32-bit cmpl vs word-sized smi) — **Deferred** (latent, needs >2^32 index).  
- Branch displacement missing guards/masking — **Deferred** (latent).

--- 
Created: 2025-10-06  
Base: 144eed2 (clean tree)  
Branch: fix/vm-overflow-hardening
