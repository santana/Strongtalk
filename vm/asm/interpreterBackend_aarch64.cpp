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
#include "code/nativeInstruction.hpp"
#include "lookup/lookupCache.hpp"
#include "memory/util.hpp"

extern "C" void scavenge_and_allocate(int size);
extern "C" void popStackHandles(char* nextFrame);
extern "C" char* nlr_testpoint_entry; // set by InterpreterGenerator::generate_nonlocal_return_code

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
  // esi keeps pointing at the bytecodes throughout the primitive call (the
  // primitive entry point is read directly from the stream); nothing to park.
}

void InterpreterBackend::restorePrimitiveBytecodePtr(MacroAssembler* masm) {}

void InterpreterBackend::passPrimitiveCallArgs(MacroAssembler* masm, int entryOffsetBytes) {
  // AAPCS64 wants the arguments in x0..x7; the delta stack slots below the
  // pushed last argument are spaced by slotSize (two oops per delta slot).
  masm->movl(x0, Address(sp, 0));
  masm->movl(x1, Address(sp, slotSize));
  masm->movl(x2, Address(sp, 2 * slotSize));
  masm->movl(x3, Address(sp, 3 * slotSize));
  masm->movl(x4, Address(sp, 4 * slotSize));
  masm->movl(x5, Address(sp, 5 * slotSize));
  masm->movl(x6, Address(sp, 6 * slotSize));
  masm->movl(x7, Address(sp, 7 * slotSize));
  masm->movl(eax, Address(esi, entryOffsetBytes)); // get primitive entry point
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

void InterpreterBackend::shiftBlockValueArgs(MacroAssembler* masm, int nArgs) {
  // call_C pushed the 16-byte return address, so the block's arguments sit one
  // slot too high for the block frame (arg1 would read [fp+16] = the saved
  // x30). Shift the nArgs down one slot so the block sees them at [fp+16].
  for (int j = nArgs; j >= 1; j--) {
    masm->movl(x0, Address(esp, j * slotSize));
    masm->movl(Address(esp, (j - 1) * slotSize), x0);
  }
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