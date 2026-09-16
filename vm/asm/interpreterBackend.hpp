/*
Copyright (c) 2026, Gerardo Santana Gomez Garrido.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef _INTERPRETER_BACKEND_HPP
#define _INTERPRETER_BACKEND_HPP

#include "asm/assembler.hpp"
#include "memory/allocation.hpp"

// InterpreterBackend centralises the architecture-specific conventions of the
// delta interpreter (the emitted bytecode engine) and of the primitives it
// calls. It mirrors the assembler split: a single shared frontend
// (interpreter.cpp, the *_gen primitives, stubRoutines.cpp) emits through the
// MacroAssembler, while this class is the one place that knows the per-arch
// shape of the delta stack and the piece of the calling convention that is
// NOT already handled by MacroAssembler::call_C.
//
// The physical interpreter frame layout (frame_*_offset in runtime/frame.hpp)
// intentionally lives elsewhere: it is shared with the GC and the frame
// walkers and is already a single per-arch source of truth. What is
// duplicated today and must not drift is the *slot model* (how a delta stack
// slot is scaled when an address is built from a register-held slot count)
// and the primitive-call/return and invocation-counter conventions, all of
// which are re-encoded in interpreter.cpp, the *_gen primitives and the
// StubRoutines.

class InterpreterBackend : public AllStatic {
public:
  // Delta stack slot model.
  //
  // On x86-64 a delta stack slot holds exactly one oop (slotSize == oopSize),
  // on AArch64 it holds two (slotSize == 2*oopSize, kept for low mem footprint
  // of the fixed-size stack regions). Register-relative addressing of stack
  // slots must therefore scale the index register by times_8 resp. times_16.
  static const Address::ScaleFactor deltaStackScale; // Address::times_8 (x86-64), Address::times_16 (AArch64)

  // [base + index*slotScale + disp] with no relocation.
  static Address slotScaled(Register base, Register index, int disp);

  // [base + index*slotScale + disp] with an explicit relocation type.
  static Address slotScaled(Register base, Register index, int disp, relocInfo::relocType rt);

  // ---- Invocation counters ----
  //
  // The methodOop invocation counter is a 32-bit field even on 64-bit
  // backends; only its upper word carries the invocation count. The accesses
  // must stay 32-bit (movl_32 / ldr_w, str_w) or the 4 padding bytes after
  // the field can make the counter look >= the limit on every method entry
  // (infinite counter-overflow loop).
  static void loadMethodCounter(MacroAssembler* masm, Register dst, Address src);
  static void storeMethodCounter(MacroAssembler* masm, Address dst, Register src);

  // Emit the method-entry "compare invocation counter to limit" sequence and
  // return the address the frontend patches through set_invocation_counter_limit.
  // limitValue is the initial raw limit; the instruction-stream embedding
  // differs per arch: x86-64 patches the imm32 field of a cmpl, AArch64 loads
  // the limit from the limitStorage global (code is not patched in place there).
  static int* emitInvocationCounterLimitTest(MacroAssembler* masm, Register counterReg, int limitValue,
                                             int* limitStorage);

  // Increment the invocation counter in place by imm (upper word). x86-64
  // folds it into the memory operand (addl mem, imm); AArch64 needs an
  // explicit load/modify/store through the scratch register x16.
  static void incrementMethodCounter(MacroAssembler* masm, Address counterAddr, int imm);

  // ---- Primitive call/return ABI ----
  //
  // call_primitive / call_primitive_can_fail push the last argument (receiver)
  // and call the generated primitive whose entry point is embedded after the
  // bytecode. On x86-64 the arguments arrive on the hardware stack and the
  // interpreter's bytecode pointer (esi = rsi) collides with the SysV second
  // argument register, so esi is parked in r12 (callee-saved) for the duration
  // of the call. On AArch64 AAPCS64 wants the arguments in x0..x7 (loaded from
  // the delta stack slots), the primitive entry point is read from the bytecode
  // stream, and esi keeps pointing at the bytecodes throughout. After the call
  // both backends pop the one pushed argument slot (caller-pops).
  static void savePrimitiveBytecodePtr(MacroAssembler* masm); // no-op on AArch64
  static void restorePrimitiveBytecodePtr(MacroAssembler* masm); // no-op on AArch64
  static void passPrimitiveCallArgs(MacroAssembler* masm, int entryOffsetBytes);

  // Receiver/argument operands of a generated primitive. x86-64: the receiver
  // is at [esp+oopSize], the argument at [esp+2*oopSize] (the primitive pops
  // both on return). AArch64: the receiver lives in x0, the argument in x1,
  // and the result must be copied from eax to x0 before ret(0). The returned
  // operand type is therefore Address on x86-64 and Register on AArch64.
#if defined(DELTA_ASSEMBLER_BACKEND_AARCH64)
  static Register primReceiver();
  static Register primArgument();
#else
  static Address primReceiver();
  static Address primArgument();
#endif
  static void returnToInterpreter(MacroAssembler* masm);
  static void returnErrorToInterpreter(MacroAssembler* masm);

  // allocateContext_var reads the context size from the hardware stack; on
  // x86-64 the return address occupies [esp], so the length slots sits at
  // [esp+oopSize], while AArch64 calls the prim with a direct blr (return
  // address in x30) and the length arrives in the top slot [esp].
  static Address contextLengthArgument();

  // Call scavenge_and_allocate with a constant or a register-held size. Does
  // not include the last-Delta-frame bookkeeping, which stays with the caller.
  static void callScavengeAndAllocate(MacroAssembler* masm, int size);
  static void callScavengeAndAllocate(MacroAssembler* masm, Register sizeReg);

  // ---- call_delta frame setup (StubRoutines::generate_call_delta) ----
  //
  // call_delta is the general Delta entry point: "extern C oop call_delta(
  // void* method, oop receiver, int nofArgs, oop* args)". The arguments arrive
  // in the native C argument registers (rdi/rsi/rdx/rcx on x86-64, x0..x3 on
  // AArch64) and are spilled into the four reserved delta-stack slots below the
  // four pushed words below ebp so the rest of the stub can read them
  // ebp-relative. The old last_Delta_fp spilled to the top slot is reloaded by
  // the caller for the stack-corruption test.
  static void spillCallDeltaArgs(MacroAssembler* masm);

  // Return to the C caller of call_delta: finish restoring the frame on the
  // way back (the caller has already reset esi/edi/last_Delta_sp/last_Delta_fp
  // and pointed esp/ebp-relative stack back at ebp) and branch to the return
  // address. The caller emits the final ret.
  static void returnToCallDeltaCaller(MacroAssembler* masm);

  // Save the C scratch registers, call popStackHandles(nextFrame) and restore
  // them (NLR-unwinding through the last C chunk).
  static void callPopStackHandles(MacroAssembler* masm);

  // ---- non-local return (NLR) stubs (StubRoutines) ----
  //
  // Decode the 32-bit ic_info word that follows a return address into a
  // full-width NLR offset: sign-extend it to 64 bits (x86-64 loads with
  // movl_32 then sign-extends via movsxq; AArch64 uses the 64-bit movl load)
  // and shift the flag bits out, leaving the NLR offset in `offset`.
  static void decodeICInfo(MacroAssembler* masm, Register offset, Address info);

  // Tail of StubRoutines::generate_continue_NLR (after the shared leave()):
  // jump to the NLR continuation from compiled code. On x86-64 the return
  // address is pushed on the stack and the continuation is computed from it
  // plus the ic_info word; on AArch64 there is no pushed return address (the
  // address lives in x30), so NLR re-enters the interpreter's NLR testpoint
  // via nlr_testpoint_entry. Clobbers ret_addr/offset.
  static void continueNonLocalReturn(MacroAssembler* masm, Register ret_addr, Register offset);

  // Enter the interrupted Delta frame in provoke_nlr_at/continue_nlr_in_delta.
  // Sets ebp/esp from the C arguments (SysV registers rdi/rsi on x86-64, the
  // two stack words [esp+oopSize]/[esp+2*oopSize] on AArch64) and loads the
  // interrupted frame's return address into ret_addr from `old_ret_addr`
  // (evaluated AFTER esp has been switched, so it reads below the new sp).
  static void enterNLRFrame(MacroAssembler* masm, Register ret_addr, Address old_ret_addr);

  // ---- Interpreted send/return conventions (interpreter.cpp) ----
  //
  // Two facts separate the backends in the method-return and NLR epilogues:
  // where the live return address lives (on the hardware stack below the frame
  // on x86-64, popped into ecx and branched to; in the link register x30 on
  // AArch64, returned via ret) and whether an interpreted send pushed the
  // receiver as an extra hardware word below the argument slots (x86-64:
  // oopSize; AArch64: none).

  // Bytes pushed by an interpreted send below its n argument slots. x86-64
  // pushes the receiver as one extra oop; AArch64 adds no such word.
  static const int interpretReceiverWordBytes; // oopSize (x86-64), 0 (AArch64)

  // ret(nArgs*slotSize + interpretReceiverWordBytes) for the fixed-arity
  // method returns (return_tos: recv_0/1/2_args). nArgs is 0..2.
  static void popArgsAndReturn(MacroAssembler* masm, int nArgs);

  // Pop countReg delta-stack slots (positive count) plus extraBytes extra stack
  // bytes and return to the interpreter. Used for the variable-arity method
  // return (return_tos: recv_n_args, extraBytes = interpretReceiverWordBytes)
  // and the NLR home-found epilogue (extraBytes = 0). x86-64 pops the return
  // address into ecx and branches; AArch64 returns through x30.
  static void popDeltaSlotsAndReturn(MacroAssembler* masm, Register countReg, int extraBytes);

  // 1s-complement `reg` to turn the NLR argument-pop count into its negative
  // "interpreted NLR" marker. x86-64 must keep the full 64-bit width (notq): a
  // 32-bit notl would zero-extend the negative marker and hide it from the
  // C-side nlr_home_id check. AArch64's notl already operates on the full
  // register.
  static void negateNLRArgumentCount(MacroAssembler* masm, Register reg);

  // ---- Context temporaries and block-value prologue (interpreter.cpp) ----

  // Byte stride of a register-held context-temporary index when building its
  // offset within contextOopDesc::temp0_byte_offset(). Historical interpreter
  // stride, preserved verbatim per arch.
  static const Address::ScaleFactor contextTempScale; // times_4 (x86-64), times_8 (AArch64)

  // primitiveValue: an AArch64 call_C pushes a 16-byte return-address slot, so
  // the i block arguments sit one delta slot too high for the block frame;
  // shift them down before following _block_entry. No-op on x86-64 (its 8-byte
  // return address does not disturb the delta-slot stride).
  static void shiftBlockValueArgs(MacroAssembler* masm, int nArgs);

  // ---- Megamorphic lookup-cache probes (interpreter.cpp megamorphic_send) ----
  //
  // Probe an element array of the global lookup cache. On entry eax holds the
  // receiver and edi the hash (recv klass ^ selector); a hit leaves the
  // resolved klass/nmethod in ecx, a klass/selector mismatch jumps to `miss`.
  // x86-64 materializes the full 64-bit cache base in eax ([reg+disp32] cannot
  // reach the static __BSS array above 4GB) and consumes edi into the element
  // byte address; AArch64 keeps edi as a scaled element index and folds the
  // absolute base into each displacement.
  static void probePrimaryLookupCache(MacroAssembler* masm, Label& miss);
  // Secondary probe: x86-64 must recompute the hash (its edi was consumed into
  // the primary element address) and, historically, does NOT re-mask; AArch64
  // reuses the still-live primary hash and applies the secondary mask.
  static void probeSecondaryLookupCache(MacroAssembler* masm, Label& miss);

  // ---- Interpreter code buffer ----

  // Size of the interpreter code buffer. AArch64 instructions are 4 bytes each
  // (vs the ~1.5-2 byte x86-64 average), so it needs a larger buffer.
  static const int interpreterCodeSize; // 40000 (x86-64), 200000 (AArch64)
};

#endif // _INTERPRETER_BACKEND_HPP