/*
Copyright (c) 2026, Gerardo Santana Gomez Garrido.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// AArch64 conventions of the InterpreterBackend. Selected by the Makefile
// (vm/asm/interpreterBackend_aarch64.cpp) when DELTA_BACKEND_AARCH64
// is defined; see asm/assembler.hpp for the backend selection macro.

#include "asm/interpreterBackend.hpp"
#include "asm/assembler_aarch64.hpp"
#include "code/nativeInstruction.hpp"
#include "lookup/lookupCache.hpp"
#include "memory/generation.inline.hpp"
#include "memory/universe.store.hpp"
#include "memory/util.hpp"
#include "oops/memOop.inline.hpp"
#include "oops/oop.inline.hpp"
#include "prims/prim.hpp"

extern "C" void st_debug_ppa2(intptr_t a0, intptr_t a1);

extern "C" void scavenge_and_allocate(int size);
extern "C" void popStackHandles(char* nextFrame);
extern "C" char* nlr_testpoint_entry; // set by InterpreterGenerator::generate_nonlocal_return_code

// Scratch callee-saved registers used by the primitive-call glue. Both are
// preserved across the C primitive call and are not part of any register map:
// the interpreter maps eax=x13, ebx=x15, ecx=x11, edx=x12, esi=x14, edi=x27,
// ebp=x29, and compiled code uses x21..x23 (local registers) and x27/x14 for
// NLR state.
static const Register x24_bytecode_ptr = x24; // parked esi across a primitive call
static const Register x25_argument_slots = x25; // delta stack slots the primitive consumes

// On AArch64 a delta stack slot holds two oops (slotSize = 2*oopSize), so a
// register-held slot count is scaled by times_16 (two oops per slot).
const Address::ScaleFactor InterpreterBackend::deltaStackScale = Address::times_16;

void InterpreterBackend::loadMethodCounter(MacroAssembler* masm, Register dst, Address src) {
  // 32-bit field: reading the 4 padding bytes after _counters would make the
  // counter look >= the limit on every method entry.
  masm->ldr_w(dst, src);
}

void InterpreterBackend::storeMethodCounter(MacroAssembler* masm, Address dst, Register src) {
  masm->str_w(src, dst);
}

int* InterpreterBackend::emitInvocationCounterLimitTest(MacroAssembler* masm, Register counterReg, int limitValue,
                                                        int* limitStorage) {
  (void)limitValue;
  // No encodable compare-with-large-immediate; load the limit from the global
  // that set_invocation_counter_limit patches (data, not executable code).
  masm->load_absolute_address(x16, Address((intptr_t)limitStorage, relocInfo::external_word_type));
  masm->ldr_w(x16, Address(x16));
  masm->cmp(counterReg, x16, LSL, 0, sz_32);
  return limitStorage;
}

void InterpreterBackend::incrementMethodCounter(MacroAssembler* masm, Address counterAddr, int imm) {
  masm->ldr_w(x16, counterAddr);
  masm->addl(x16, imm);
  masm->str_w(x16, counterAddr);
}

void InterpreterBackend::savePrimitiveBytecodePtr(MacroAssembler* masm) {
  // A primitive runs as a C call, so it may clobber any caller-saved register,
  // including esi (x14, the bytecode pointer). Park it in x24 (callee-saved and
  // unused by the interpreter/compiler register maps) for the duration of the
  // call, exactly like the x86-64 backend parks esi in r12.
  masm->mov(x24_bytecode_ptr, esi);
}

void InterpreterBackend::restorePrimitiveBytecodePtr(MacroAssembler* masm) {
  masm->mov(esi, x24_bytecode_ptr);
}

void InterpreterBackend::passPrimitiveCallArgs(MacroAssembler* masm, int entryOffsetBytes) {
  // The primitive consumes number_of_parameters() delta stack slots (the
  // compiler pushed all but the last operand; call_primitive/call_primitive_can_fail
  // push the last one). Resolve the count from the primitive's entry point now,
  // while the argument registers x0..x7 are still free, and keep it in x25
  // (callee-saved, preserved across the primitive call) for
  // popPrimitiveCallArguments. This is what the historic __stdcall "ret n"
  // did on the callee side.
  masm->movl(x0, Address(x24_bytecode_ptr, entryOffsetBytes)); // primitive entry point
  masm->mov(x19, x0); // stash function pointer in x19 (callee-saved, survives call_C)
  masm->call_C((char*)&primitives::number_of_argument_slots, x0); // -> eax
  masm->movl(x25_argument_slots, eax);

  // AAPCS64 wants the arguments in x0..x7. Delta stack pushes oops at oopSize
  // (8-byte) intervals, not slotSize (16-byte). PRIM_DECL_N reverses parameter
  // order (PRIM_DECL_2(a1,a2) -> name(a2,a1)), so x0 = last arg, x1 =
  // next-to-last, ..., xN-1 = receiver (first arg).
  // x25 = (nparams + 1) * oopSize (bytes to pop, incl. pushl(eax) dup).
  // Stack after pushl(eax): [sp+0]=last arg dup, [sp+1*oopSize]=last arg,
  //   [sp+2*oopSize]=argN-1, ..., [sp+nparams*oopSize]=receiver (arg 0).
  // Walk UP from sp+oopSize to load args in PRIM_DECL_N reversed order:
  // x0 = last arg [sp+oopSize], x1 = argN-1 [sp+2*oopSize], ...,
  // xN-1 = receiver [sp+nparams*oopSize].
  masm->ldr(x0, Address(sp, oopSize)); // x0 = last arg [sp+oopSize]
  masm->ldr(x1, Address(sp, 2 * oopSize)); // x1 = argN-1 [sp+2*oopSize]
  masm->ldr(x2, Address(sp, 3 * oopSize)); // x2 = argN-2 [sp+3*oopSize]
  masm->ldr(x3, Address(sp, 4 * oopSize)); // x3 = argN-3 [sp+4*oopSize]
  masm->ldr(x4, Address(sp, 5 * oopSize)); // x4 = argN-4 [sp+5*oopSize]
  masm->ldr(x5, Address(sp, 6 * oopSize)); // x5 = argN-5 [sp+6*oopSize]
  masm->ldr(x6, Address(sp, 7 * oopSize)); // x6 = argN-6 [sp+7*oopSize]
  masm->ldr(x7, Address(sp, 8 * oopSize)); // x7 = argN-7 [sp+8*oopSize]
  // Function pointer stays in x19 (loaded at line 92); callPrimitive will blr(x19).
}

void InterpreterBackend::callPrimitive(MacroAssembler* masm) {
  // Save VM registers that the C call may clobber (AAPCS64 caller-saved).
  masm->str_pre(x14, sp, -16); // save esi
  masm->str_pre(x15, sp, -16); // save ebx
  masm->str_pre(x10, sp, -16); // save edi
  masm->str_pre(x16, sp, -16); // save ebp
  masm->blr(x19); // call primitive via function pointer in x19
  masm->ldr_post(x16, sp, 16); // restore ebp
  masm->ldr_post(x10, sp, 16); // restore edi
  masm->ldr_post(x15, sp, 16); // restore ebx
  masm->ldr_post(x14, sp, 16); // restore esi
  // Result in x0 -> move to eax (x13)
  masm->mov(eax, x0);
}

void InterpreterBackend::popPrimitiveCallArguments(MacroAssembler* masm) {
  // Pop the operand slots the primitive consumed (computed by
  // passPrimitiveCallArgs above and kept in x25).
  //
  // sp is register 31, which the shifted-register ADD form reads/writes as xzr;
  // only the immediate form aliases 31 to sp. The count is only known at run
  // time, so adjust sp through the reserved scratch register x16.
  masm->mov(x16, esp);
  masm->addl(x16, x25_argument_slots); // x25 already in bytes (nparams * slotSize)
  masm->mov(esp, x16);
}

Register InterpreterBackend::primReceiver() {
  return x0;
}

Register InterpreterBackend::primArgument() {
  return x1;
}

void InterpreterBackend::returnToInterpreter(MacroAssembler* masm) {
  // call_C copies x0 -> eax after the C call, so the result must be in x0.
  masm->mov(x0, eax);
  masm->ret(0);
}

void InterpreterBackend::returnErrorToInterpreter(MacroAssembler* masm) {
  masm->mov(x0, eax);
  masm->ret(0);
}

void InterpreterBackend::copyResultToReturnRegister(MacroAssembler* masm) {
  // call_C(Register) copies x0 -> eax after the callee returns; publish the
  // interpreter's tos (eax) in x0 first so a block invoked through
  // primitiveValue doesn't lose its result.
  masm->mov(x0, eax);
}

Address InterpreterBackend::contextLengthArgument() {
  // install_context calls this prim with a direct blr, so the return address
  // lives in x30 rather than on the stack and the length sits in the top slot.
  return Address(esp, 0);
}

void InterpreterBackend::callScavengeAndAllocate(MacroAssembler* masm, int size) {
  masm->movl(edx, size); // AAPCS64 argument passed via call_C
  masm->call_C((char*)&scavenge_and_allocate, edx); // result copied back to eax
}

void InterpreterBackend::callScavengeAndAllocate(MacroAssembler* masm, Register sizeReg) {
  masm->call_C((char*)&scavenge_and_allocate, sizeReg); // result copied back to eax
}

void InterpreterBackend::spillCallDeltaArgs(MacroAssembler* masm) {
  // Reserve the four argument slots below the four pushed words below ebp and
  // spill the register-passed arguments into them (see generate_call_delta).
  masm->addq(esp, -4 * slotSize);
  masm->movq(Address(esp, 0), x0); // method
  masm->movq(Address(esp, slotSize), x1); // receiver
  masm->movl(Address(esp, 2 * slotSize), x2); // nofArgs
  masm->movq(Address(esp, 3 * slotSize), x3); // args
}

void InterpreterBackend::returnToCallDeltaCaller(MacroAssembler* masm) {
  // eax is x13, a scratch across the C boundary; move the result to x0 and
  // restore the frame/link saved by enter() (the shared ret(0) follows).
  masm->mov(x0, eax);
  masm->leave();
}

void InterpreterBackend::callPopStackHandles(MacroAssembler* masm) {
  masm->pushl(eax);
  masm->pushl(ebx);
  masm->pushl(edx);
  masm->pushl(edi);
  masm->pushl(esi);
  masm->pushl(ecx);
  masm->call((char*)&popStackHandles, relocInfo::external_word_type);
  masm->popl(ecx);
  masm->popl(esi);
  masm->popl(edi);
  masm->popl(edx);
  masm->popl(ebx);
  masm->popl(eax);
}

void InterpreterBackend::decodeICInfo(MacroAssembler* masm, Register offset, Address info) {
  masm->movl(offset, info);
  if (IC_Info::number_of_flags > 0) {
    masm->sarl(offset, IC_Info::number_of_flags); // shift ic info flags out
  }
}

void InterpreterBackend::continueNonLocalReturn(MacroAssembler* masm, Register ret_addr, Register offset) {
  (void)offset;
  masm->load_absolute_value(ret_addr, Address((intptr_t)&nlr_testpoint_entry, relocInfo::external_word_type));
  masm->br(ret_addr); // do NLR
}

void InterpreterBackend::enterNLRFrame(MacroAssembler* masm, Register ret_addr, Address old_ret_addr) {
  masm->movl(ebp, Address(esp, oopSize)); // set new frame pointer
  masm->movl(esp, Address(esp, 2 * oopSize)); // set new stack pointer
  masm->movl(ret_addr, old_ret_addr); // find old return address
}

// ---- Interpreted send/return conventions ----

const int InterpreterBackend::interpretReceiverWordBytes = 0;

void InterpreterBackend::popArgsAndReturn(MacroAssembler* masm, int nArgs) {
  masm->ret(nArgs * slotSize + InterpreterBackend::interpretReceiverWordBytes);
}

void InterpreterBackend::popDeltaSlotsAndReturn(MacroAssembler* masm, Register countReg, int extraBytes) {
  (void)extraBytes;
  masm->leal(esp, Address(esp, countReg, deltaStackScale)); // remove arguments
  masm->ret(0); // return (the link lives in x30, not on the stack)
}

void InterpreterBackend::negateNLRArgumentCount(MacroAssembler* masm, Register reg) {
  masm->notl(reg); // make negative to distinguish from compiled NLRs (64-bit already)
}

// ---- Context temporaries / block values ----

const Address::ScaleFactor InterpreterBackend::contextTempScale = Address::times_8;

void InterpreterBackend::setupBlockValueFrame(MacroAssembler* masm, int nArgs) {
  // On entry sp points at the saved-LR slot that call_C pushed; the block
  // arguments are at [sp + j*slotSize] (j = 1..nArgs, arg1 = the last formal)
  // and call_C's saved LR is at [sp]. The block's return bytecode pops
  // (nArgs+1) delta slots, so if the arguments stayed put the block frame
  // would sit on top of the saved LR and the return would lift sp one slot
  // above it -- RESTORE_LR would then reload x30 from garbage and call_C would
  // leave sp unbalanced. Instead place the whole block frame below the saved
  // LR: move each argument down (nArgs+2) slots and lower sp one frame (nArgs+1
  // slots). enter() then makes fp = sp - 16, arg1 lands at [fp+16], and the
  // block's leave + pop(nArgs+1) restores sp to exactly the saved LR.
  //
  // Walk upward: every destination is below sp while every source is above it,
  // so no store can clobber a not-yet-read argument. x0 is scratch -- the
  // block closure is carried in eax (x13) and the block prologue reloads x0.
  for (int j = 1; j <= nArgs; j++) {
    masm->movl(x0, Address(esp, j * slotSize));
    masm->movl(Address(esp, (j - nArgs - 2) * slotSize), x0);
  }
  masm->addl(esp, -(nArgs + 1) * slotSize);
}

// ---- Megamorphic lookup-cache probes ----

// log2(4 * oopSize): the cache element stride is 4 words regardless of backend.
static int lookupCacheElementShift() {
  int shift = 0;
  for (int s = 4 * oopSize; s > 1; s >>= 1)
    shift++;
  return shift;
}

static void probeLookupCacheElement(MacroAssembler* masm, Register indexReg, intptr_t base, Label& miss) {
  masm->cmpl(ecx, Address(indexReg, base + 0 * oopSize)); // compare element.klass
  masm->jcc(Assembler::notEqual, miss);
  masm->cmpl(edx, Address(indexReg, base + 1 * oopSize)); // compare element.selector
  masm->jcc(Assembler::notEqual, miss);
  masm->movl(ecx, Address(indexReg, base + 2 * oopSize)); // load resolved klass/nmethod
}

void InterpreterBackend::probePrimaryLookupCache(MacroAssembler* masm, Label& miss) {
  probeLookupCacheElement(masm, edi, lookupCache::primary_cache_address(), miss);
}

void InterpreterBackend::probeSecondaryLookupCache(MacroAssembler* masm, Label& miss) {
  masm->andl(edi, (secondary_cache_size - 1) << lookupCacheElementShift());
  probeLookupCacheElement(masm, edi, lookupCache::secondary_cache_address(), miss);
}

const int InterpreterBackend::interpreterCodeSize = 200000;