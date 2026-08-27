/*
Copyright (c) 2026, Gerardo Santana Gomez Garrido.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// x86/x86-64 implementation of the Mapping interface (see asm/mapping.hpp).
// This is the sibling of asm/mapping_aarch64.cpp; the register conventions
// are documented in asm/mapping_x86.hpp.
//
// This file is inert unless the AArch64 backend is NOT selected (i.e. the
// default x86 build).

#if defined(DELTA_COMPILER) && !defined(DELTA_ASSEMBLER_BACKEND_AARCH64)

#include "asm/codeBuffer.hpp"
#include "asm/mapping.hpp"
#include "compiler/compiler.hpp"
#include "compiler/preg.hpp"
#include "oops/blockOop.hpp"

// stack mapping

void Mapping::initialize() {
  _localRegisters[0] = asLocation(localReg0);
  _localRegisters[1] = asLocation(localReg1);
  _localRegisters[2] = asLocation(localReg2);
  int i;
  for (i = 0; i < nofRegisters     ; i++) _localRegisterIndex[i] = -1;
  for (i = 0; i < nofLocalRegisters; i++) _localRegisterIndex[_localRegisters[i].number()] = i;
  for (i = 0; i < nofLocalRegisters; i++) {
    Register r = asRegister(_localRegisters[i]);
    assert((r != temp1) && (r != temp2) && (r != temp3), "local registers must be disjoint from temporary registers");
  }
}


// local registers
Location Mapping::_localRegisters[nofLocalRegisters + 1];
int Mapping::_localRegisterIndex[nofRegisters + 1];

Location Mapping::localRegister(int i) {
  assert(0 <= i && i < nofLocalRegisters, "illegal local register index");
  return _localRegisters[i];
}


int Mapping::localRegisterIndex(Location l) {
  assert(0 <= l.number() && l.number() < nofRegisters, "illegal local register");
  int res = _localRegisterIndex[l.number()];
  assert(res >= 0, "not a local register");
  assert(localRegister(res) == l, "incorrect mapping");
  return res;
}


// parameter passing
// On x86/x86-64, all Delta arguments are passed on the stack.
Location Mapping::incomingArg(int i, int nofArgs) {
  assert((0 <= i) && (i < nofArgs), "illegal arg number");
  return Location::stackLocation(i);
}


Location Mapping::outgoingArg(int i, int nofArgs) {
  assert((0 <= i) && (i < nofArgs), "illegal arg number");
  return Location::stackLocation(i);
}


// stack allocation (Note: offsets are always in oops!)
Location Mapping::localTemporary(int i) {
  assert(i >= 0, "illegal temporary number");
  int floats = theCompiler->totalNofFloatTemporaries();
  int offset = (floats > 0 ? first_float_offset - floats*(floatSize/oopSize) : first_temp_offset) - i;
  return Location::stackLocation(offset);
}


int Mapping::localTemporaryIndex(Location l) {
  int floats = theCompiler->totalNofFloatTemporaries();
  int i = (floats > 0 ? first_float_offset - floats*(floatSize/oopSize) : first_temp_offset) - l.offset();
  assert(localTemporary(i) == l, "incorrect mapping");
  return i;
}


Location Mapping::floatTemporary(int scope_id, int i) {
  InlinedScope* scope = theCompiler->scopes->at(scope_id);
  assert(scope->firstFloatIndex() >= 0, "firstFloatIndex not computed yet");
  assert(floatSize == 2*oopSize, "check this code");
  Location loc = Location::stackLocation(first_float_offset - (scope->firstFloatIndex() + i)*(floatSize/oopSize));
  assert((loc.offset() * oopSize) % floatSize == 0, "offset is not correctly aligned");
  return loc;
}


// context temporaries
Location Mapping::contextTemporary(int contextNo, int i, int scope_offset) {
  assert((0 <= contextNo) && (0 <= i), "illegal context or temporary no");
  return Location::compiledContextLocation(contextNo, i, scope_offset);
}


Location* Mapping::new_contextTemporary(int contextNo, int i, int scope_id) {
  assert((0 <= contextNo) && (0 <= i), "illegal context or temporary no");
  return new Location(contextLoc1, contextNo, i, scope_id);
}


int Mapping::contextOffset(int tempNo) {
  return tempNo*oopSize + contextOopDesc::temp0_byte_offset();
}


// predicates
bool Mapping::isNormalTemporary(Location loc) {
  assert(!loc.isFloatLocation(), "must have been converted into stackLoc by register allocation");
  return loc.isStackLocation() && !isFloatTemporary(loc);
}


bool Mapping::isFloatTemporary(Location loc) {
  assert(!loc.isFloatLocation(), "must have been converted into stackLoc by register allocation");
  if (!loc.isStackLocation()) return false;
  int floats = theCompiler->totalNofFloatTemporaries();
  int offset = loc.offset();
  return floats > 0 && first_float_offset + 2 >= offset && offset > first_float_offset - floats*(floatSize/oopSize);
}


// helper functions for code generation
// x86-64 uses movq for pointer-sized ops; x86-32 uses movl.
#if DELTA_X86_64
  #define MAPPING_MOVQ(d, s)  theMacroAssm->movq(d, s)
  #define MAPPING_MOVL(d, s) theMacroAssm->movq(d, s)
  #define MAPPING_MOVL_MEM(d, s) theMacroAssm->movq(d, s)
  #define MAPPING_MOVL_IMM(d, v) theMacroAssm->movq(d, (intptr_t)(v))
#else
  #define MAPPING_MOVQ(d, s)  theMacroAssm->movl(d, s)
  #define MAPPING_MOVL(d, s) theMacroAssm->movl(d, s)
  #define MAPPING_MOVL_MEM(d, s) theMacroAssm->movl(d, s)
  #define MAPPING_MOVL_IMM(d, v) theMacroAssm->movl(d, (intptr_t)(v))
#endif

void Mapping::load(Location src, Register dst) {
  switch (src.mode()) {
    case specialLoc: {
      if (src == resultOfNLR) {
        if (NLR_result_reg != dst) MAPPING_MOVQ(dst, NLR_result_reg);
      } else {
        ShouldNotReachHere();
      }
      break;
    }
    case registerLoc: {
      Register s = asRegister(src);
      if (s != dst) MAPPING_MOVQ(dst, s);
      break;
    }
    case stackLoc: {
      assert(isNormalTemporary(src), "must be a normal temporary location");
      MAPPING_MOVL_MEM(dst, Address(frame_reg, src.offset() * oopSize));
      break;
    }
    case contextLoc1: {
      PReg* base = theCompiler->contextList->at(src.contextNo())->context();
      load(base->loc, dst);
      MAPPING_MOVL_MEM(dst, Address(dst, contextOffset(src.tempNo())));
      break;
    }
    default: {
      ShouldNotReachHere();
      break;
    }
  }
}


void Mapping::store(Register src, Location dst, Register temp1, Register temp2, bool needsStoreCheck) {
  assert(src != temp1 && src != temp2 && temp1 != temp2, "registers must be different");
  switch (dst.mode()) {
    case specialLoc: {
      if (dst == topOfStack) {
        theMacroAssm->pushl(src);
      } else {
        ShouldNotReachHere();
      }
      break;
    }
    case registerLoc: {
      Register d = asRegister(dst);
      if (d != src) MAPPING_MOVQ(d, src);
      break;
    }
    case stackLoc: {
      assert(isNormalTemporary(dst), "must be a normal temporary location");
      MAPPING_MOVL_MEM(Address(frame_reg, dst.offset()*oopSize), src);
      break;
    }
    case contextLoc1: {
      PReg* base = theCompiler->contextList->at(dst.contextNo())->context();
      load(base->loc, temp1);
      MAPPING_MOVL_MEM(Address(temp1, contextOffset(dst.tempNo())), src);
      if (needsStoreCheck) theMacroAssm->store_check(temp1, temp2);
      break;
    }
    default: {
      ShouldNotReachHere();
      break;
    }
  }
}


void Mapping::storeO(oop obj, Location dst, Register temp1, Register temp2, bool needsStoreCheck) {
  assert(temp1 != temp2, "registers must be different");
  switch (dst.mode()) {
    case specialLoc: {
      if (dst == topOfStack) {
        MAPPING_MOVL_IMM(temp1, obj);
        theMacroAssm->pushl(temp1);
      } else {
        ShouldNotReachHere();
      }
      break;
    }
    case registerLoc: {
      MAPPING_MOVL_IMM(asRegister(dst), obj);
      break;
    }
    case stackLoc: {
      assert(isNormalTemporary(dst), "must be a normal temporary location");
      MAPPING_MOVL_IMM(temp1, obj);
      MAPPING_MOVL_MEM(Address(frame_reg, dst.offset()*oopSize), temp1);
      break;
    }
    case contextLoc1: {
      PReg* base = theCompiler->contextList->at(dst.contextNo())->context();
      load(base->loc, temp1);
      MAPPING_MOVL_IMM(temp2, obj);
      MAPPING_MOVL_MEM(Address(temp1, contextOffset(dst.tempNo())), temp2);
      if (needsStoreCheck) theMacroAssm->store_check(temp1, temp2);
      break;
    }
    default: {
      ShouldNotReachHere();
      break;
    }
  }
}


void Mapping::fload(Location src, Register base) {
  if (src == topOfFloatStack) {
    if (UseFPUStack) {
      // nothing to do, value is on the FPU stack already
    } else {
      ShouldNotReachHere();
    }
  } else {
    // Non-FPU-stack path not implemented for x86 (UseFPUStack is always true)
    ShouldNotReachHere();
  }
}


void Mapping::fstore(Location dst, Register base) {
  if (dst == topOfFloatStack) {
    if (UseFPUStack) {
      // nothing to do, value is on the FPU stack already
    } else {
      ShouldNotReachHere();
    }
  } else {
    ShouldNotReachHere();
  }
}


void mapping_init() {
  Mapping::initialize();
}

#endif // DELTA_COMPILER && !DELTA_ASSEMBLER_BACKEND_AARCH64
