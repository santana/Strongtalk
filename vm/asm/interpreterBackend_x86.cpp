/*
Copyright (c) 2026, Gerardo Santana Gomez Garrido.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// x86-64 conventions of the InterpreterBackend. Selected by the Makefile
// (vm/asm/interpreterBackend_x86.cpp) when DELTA_ASSEMBLER_BACKEND_AARCH64
// is not defined; see asm/assembler.hpp for the backend selection macro.

#include "asm/interpreterBackend.hpp"
#include "code/nativeInstruction.hpp"
#include "lookup/lookupCache.hpp"
#include "memory/util.hpp"

extern "C" void scavenge_and_allocate(int size);
extern "C" void popStackHandles(char* nextFrame);

// On x86-64 a delta stack slot is exactly one oop (slotSize == oopSize), so a
// register-held slot count is scaled by times_8 (one oop per slot).
const Address::ScaleFactor InterpreterBackend::deltaStackScale = Address::times_8;

// ICD_ACCESS / macro assembler glue shared with the primitives.
void InterpreterBackend::loadMethodCounter(MacroAssembler* masm, Register dst, Address src) {
  masm->movl_32(dst, src);
}

void InterpreterBackend::storeMethodCounter(MacroAssembler* masm, Address dst, Register src) {
  masm->movl_32(dst, src);
}

int* InterpreterBackend::emitInvocationCounterLimitTest(MacroAssembler* masm, Register counterReg, int limitValue,
                                                        int* limitStorage) {
  (void)limitStorage;
  // Patchable imm32 field of "cmp <counterReg>, imm32"; set_invocation_counter_limit
  // rewrites it in place (asserts on the 0x81 opcode).
  masm->cmpl(counterReg, limitValue);
  return (int*)(masm->pc() - sizeof(int));
}

void InterpreterBackend::incrementMethodCounter(MacroAssembler* masm, Address counterAddr, int imm) {
  masm->addl(counterAddr, imm);
}

void InterpreterBackend::savePrimitiveBytecodePtr(MacroAssembler* masm) {
  // esi == rsi collides with the SysV argument #2 register; park the bytecode
  // pointer in r12 (callee-saved, preserved across the C call).
  masm->movq(r12, esi);
}

void InterpreterBackend::restorePrimitiveBytecodePtr(MacroAssembler* masm) {
  masm->movq(esi, r12);
}

void InterpreterBackend::passPrimitiveCallArgs(MacroAssembler* masm, int entryOffsetBytes) {
  // Load the primitive entry point from the saved bytecode pointer (before
  // esi is clobbered by the argument shuffling below).
  masm->movl(eax, Address(r12, entryOffsetBytes));
  // SysV AMD64 ABI: first six arguments in rdi, rsi, rdx, rcx, r8, r9.
  masm->movq(edi, Address(esp, 0 * oopSize));
  masm->movq(esi, Address(esp, 1 * oopSize));
  masm->movq(edx, Address(esp, 2 * oopSize));
  masm->movq(ecx, Address(esp, 3 * oopSize));
  masm->movq(r8, Address(esp, 4 * oopSize));
  masm->movq(r9, Address(esp, 5 * oopSize));
}

Address InterpreterBackend::primReceiver() {
  return Address(esp, oopSize);
}

Address InterpreterBackend::primArgument() {
  return Address(esp, 2 * oopSize);
}

void InterpreterBackend::returnToInterpreter(MacroAssembler* masm) {
  // The 8-byte return address is on top of the stack, so the pushed receiver
  // and argument are popped by ret (historic 32-bit offsets were [esp+4]/[esp+8]).
  masm->ret(2 * oopSize);
}

void InterpreterBackend::returnErrorToInterpreter(MacroAssembler* masm) {
  masm->ret(2 * oopSize);
}

Address InterpreterBackend::contextLengthArgument() {
  return Address(esp, +oopSize);
}

void InterpreterBackend::callScavengeAndAllocate(MacroAssembler* masm, int size) {
  masm->movl(edi, size); // SysV AMD64: first argument in rdi
  masm->call_C((char*)&scavenge_and_allocate, relocInfo::runtime_call_type);
}

void InterpreterBackend::callScavengeAndAllocate(MacroAssembler* masm, Register sizeReg) {
  masm->movl(edi, sizeReg); // SysV AMD64: first argument in rdi
  masm->call((char*)&scavenge_and_allocate, relocInfo::runtime_call_type);
}

void InterpreterBackend::spillCallDeltaArgs(MacroAssembler* masm) {
  // Reserve the four argument slots below the four pushed words below ebp and
  // spill the register-passed arguments into them (see generate_call_delta).
  masm->subq(esp, 4 * oopSize);
  masm->movq(Address(esp, 0), edi); // method (rdi)
  masm->movq(Address(esp, oopSize), esi); // receiver (rsi)
  masm->movl(Address(esp, 2 * oopSize), edx); // nofArgs
  masm->movq(Address(esp, 3 * oopSize), ecx); // args (rcx)
}

void InterpreterBackend::returnToCallDeltaCaller(MacroAssembler* masm) {
  // esp points at the ebp saved by enter(); pop it back and return to the
  // caller (the shared ret(0) follows).
  masm->popq(ebp);
}

void InterpreterBackend::callPopStackHandles(MacroAssembler* masm) {
  masm->movq(edi, ecx); // SysV arg1 = nextFrame (from the pushed _last_Delta_sp)
  masm->pushq(eax);
  masm->pushq(ebx);
  masm->pushq(edx);
  masm->pushq(ecx);
  masm->pushq(esi);
  masm->pushq(edi);
  masm->call((char*)&popStackHandles, relocInfo::external_word_type);
  masm->popq(edi);
  masm->popq(esi);
  masm->popq(ecx);
  masm->popq(edx);
  masm->popq(ebx);
  masm->popq(eax);
}

void InterpreterBackend::decodeICInfo(MacroAssembler* masm, Register offset, Address info) {
  masm->movl_32(offset, info); // 32-bit load (zero-extends the ic_info word)
  masm->movsxq(offset, offset); // sign-extend the ic_info word to 64 bits
  if (IC_Info::number_of_flags > 0) {
    masm->sarl(offset, IC_Info::number_of_flags); // shift ic info flags out
  }
}

void InterpreterBackend::continueNonLocalReturn(MacroAssembler* masm, Register ret_addr, Register offset) {
  masm->popl(ret_addr); // get (local) return address
  InterpreterBackend::decodeICInfo(masm, offset, Address(ret_addr, IC_Info::info_offset)); // get ic info
  masm->addl(ret_addr, offset); // compute non-local return address
  masm->jmp(ret_addr); // do NLR
}

void InterpreterBackend::enterNLRFrame(MacroAssembler* masm, Register ret_addr, Address old_ret_addr) {
  masm->movq(ebp, edi); // set new frame pointer (from SysV arg1)
  masm->movq(esp, esi); // set new stack pointer (from SysV arg2)
  masm->movq(ret_addr, old_ret_addr); // find old return address
}

// ---- Interpreted send/return conventions ----

const int InterpreterBackend::interpretReceiverWordBytes = oopSize;

void InterpreterBackend::popArgsAndReturn(MacroAssembler* masm, int nArgs) {
  masm->ret(nArgs * slotSize + InterpreterBackend::interpretReceiverWordBytes);
}

void InterpreterBackend::popDeltaSlotsAndReturn(MacroAssembler* masm, Register countReg, int extraBytes) {
  masm->popl(ecx); // get return address
  masm->leal(esp, Address(esp, countReg, deltaStackScale)); // remove arguments
  if (extraBytes != 0) {
    masm->addl(esp, extraBytes); // also remove the receiver word (see recv_n_args note)
  }
  masm->jmp(ecx); // return
}

void InterpreterBackend::negateNLRArgumentCount(MacroAssembler* masm, Register reg) {
  // 64-bit NOT: nlr_home_id is an intptr_t, so the 1s-complemented argument
  // count must stay negative (a 32-bit NOT would zero-extend and read back
  // positive, hiding the "interpreted NLR" marker).
  masm->notq(reg);
}

// ---- Context temporaries / block values ----

const Address::ScaleFactor InterpreterBackend::contextTempScale = Address::times_4;

void InterpreterBackend::shiftBlockValueArgs(MacroAssembler* masm, int nArgs) {
  (void)masm;
  (void)nArgs;
  // No-op: an 8-byte return-address slot does not disturb the delta-slot stride.
}

// ---- Megamorphic lookup-cache probes ----

static void probeLookupCacheElement(MacroAssembler* masm, Register indexReg, intptr_t /*base*/, Label& miss) {
  masm->cmpl(ecx, Address(indexReg, 0 * oopSize)); // compare element.klass
  masm->jcc(Assembler::notEqual, miss);
  masm->cmpl(edx, Address(indexReg, 1 * oopSize)); // compare element.selector
  masm->jcc(Assembler::notEqual, miss);
  masm->movl(ecx, Address(indexReg, 2 * oopSize)); // load resolved klass/nmethod
}

void InterpreterBackend::probePrimaryLookupCache(MacroAssembler* masm, Label& miss) {
  masm->pushq(eax); // save receiver
  masm->movq(eax, lookupCache::primary_cache_address()); // full 64-bit cache base
  masm->addq(edi, eax); // edi = full element byte address
  masm->popq(eax); // restore receiver
  probeLookupCacheElement(masm, edi, 0, miss);
}

void InterpreterBackend::probeSecondaryLookupCache(MacroAssembler* masm, Label& miss) {
  // edi holds the primary element *address*; recompute the byte offset from the
  // still-live klass (ecx) and selector (edx), then fold the full 64-bit base.
  masm->movl(edi, ecx);
  masm->xorl(edi, edx);
  masm->pushq(eax); // save receiver
  masm->movq(eax, lookupCache::secondary_cache_address()); // full 64-bit cache base
  masm->addq(edi, eax); // edi = full element byte address
  masm->popq(eax); // restore receiver
  probeLookupCacheElement(masm, edi, 0, miss);
}

const int InterpreterBackend::interpreterCodeSize = 40000;