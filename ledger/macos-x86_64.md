# macOS x86-64 (forced)

**Platform**: macOS x86_64 forced (`make ARCH=x86_64`)  
**Build**: `build/x86_64-macos-clang`  
**Commit**: 144eed2 (HEAD as of capture)

## Status

| Aspect | Status | Notes |
|---|---|---|
| Build | OK | Builds clean for x86-64 backend. |
| Image load | OK | Reads `strongtalk.bst`, image-compat rewrites applied. |
| Runtime | OK (REPL) | Boots, runs image error handler machinery and reaches `Eval>` prompt. Basic Smalltalk expressions evaluate correctly at the REPL (via the `^` send syntax in the evaluator). |
| GC / allocation | Mixed | Small array allocations succeed; larger allocations (e.g. ~700000 elements) can trigger GC/scavenge faults under certain paths (observed SIGSEGV during scavenge of large arrays in interactive runs). Needs further diagnosis. |

## Current Behavior

With stdin closed or minimal input, the VM:

1. Prints banner and loads image.
2. Executes image's error-handling machinery (`Error>>defaultAction`, `BlockExceptionHandler`, etc.) and reaches the `Eval>` prompt.
3. Accepts evaluator input. Examples observed:
- `^ 3 + 4` → returns `7`
- `^ Object new class name` → returns the class
- `^ Smalltalk version` / other sends may still hit normal Smalltalk errors/DoesNotUnderstand paths (handled by image)
- Large allocations like `^ Array new: 700000` can cause a SIGSEGV in the scavenge/mark path (backtrace shows `memOopDesc::is_forwarded()` / scavenge closure iterating frames)

The evaluator correctly parses/dispatches messages and prints results. The tokenizer treats `^` as "evaluate expression"; raw expressions without `^` go through different parsing paths.

## Evidence

- Captured REPL sessions via pty driving: prompt appears, expressions evaluated successfully.
- Stack traces show normal exception handling flow reaching `Eval>`.
- Crash observed on large array allocation: fault_addr 0x0 in `memOopDesc::is_forwarded()` called from scavenge during GC triggered by allocation.
- Eden/heap sizes are 512K-words+ (EdenSize 512Kwords) — large objects stress GC.

## Known Issues / References

- Shared: interpreter receiver-slot decode passes SMI-tagged word in send path historically noted (x86-64 X15 in ARCHITECTURE.md/README.md). Current REPL behavior suggests the "every send becomes silent no-op" class of failure is not occurring in this configuration at startup; the REPL is functional.
- GC fault on large allocations is a separate runtime issue (heap/forwarding/scavenge) observed in this capture.

## Reproduction

```sh
cd build/x86_64-macos-clang
DYLD_LIBRARY_PATH=. ./strongtalk -b ../../strongtalk.bst
# then at Eval>: ^ 3 + 4
```
