# Strongtalk Architecture Ledger

This ledger documents the current state, known issues, and reproduction notes for each supported architecture/platform configuration in Strongtalk. Each platform has its own detailed document.

## Contents

- [macOS arm64 (AArch64) — macOS on Apple Silicon](macos-arm64.md)
- [macOS x86-64 (Intel) — Forced on macOS host](macos-x86_64.md)
- [Linux x86-64](linux-x86_64.md)
- [Windows x86-64 (MinGW)](windows-x86_64.md)

## Root Cause Summary

The remaining shared blocker reported across both macOS arches is that the interpreter's receiver-slot decode for sends passes a non-pointer (an SMI-tagged word) to `InterpretedIC::inline_cache_miss()`, causing a bogus `receiver->klass()` dereference. The AArch64 path is currently hanging after image load (post the AArch64 self_offset fix from commit 144eed2), while x86-64 successfully boots to the `Eval>` REPL. Linux and Windows have their own observed failure modes as documented below.

See also: [README.md](../README.md), [ARCHITECTURE.md](../ARCHITECTURE.md)

## Cross-cutting Findings

- **Shared root cause (documented)**: Interpreter receiver-slot decode for sends hands `InterpretedIC::inline_cache_miss()` a non-pointer (SMI-tagged) where receiver should be on both macOS arches (AArch64 A8, x86-64 X15). This is the VM send-path issue highlighted in README/ARCHITECTURE.
- **macOS x86-64**: functional REPL; large allocations can trigger GC faults (e.g. scavenge path accessing forwarding/mark state). Tokenizer accepts `^` prefix in evaluator; parenthesized expressions may not parse in the non-`^` form as written.
- **macOS arm64**: currently hangs after image load (post 144eed2). Heavy IC misses during startup; event/transfer coordination blocks.
- **Linux x86-64**: reaches image error machinery but is overwhelmed by continuous register dumps from signal/context logging; effectively unusable in this state.
- **Windows x86-64 (MinGW)**: image loads and the VM boots cleanly under native amd64 wine (QEMU TCG + wine, `setarch x86_64 -R`): the image-compat patches run through the real interpreter to `exit=0` with no access violation. Fixes: LLP64 `bits.hpp` mask truncation (32-bit `~0UL` zeroed address upper halves), RIP-relative disp32 wrap on `external_word` C-global refs (`export_pcrel` loads), and the missing Windows x64 C-call shadow space + arg-overlap staging in `call_C`. Remaining: `stest.exe` exits `0xC0000005` after 3 init `call_generic`s; DNU-tail pops with a 32-bit byte count; DLL-call stubs / compiler C-call sites still lack shadow space. Rosetta-only smoke faults `rosetta error: invalid gdt selector index 5` (emulation artifact).
