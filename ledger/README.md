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
- **Windows x86-64 (MinGW)**: fails early with `oldSpace::object_start` assertion `contains(q)` during heap traversal after image load (smoke test under Wine).
