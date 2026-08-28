/* Copyright (c) 2007, Strongtalk Group
All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the 
following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following 
	  disclaimer in the documentation and/or other materials provided with the distribution.
    * Neither the name of the Strongtalk Group nor the names of its contributors may be used to endorse or promote products derived 
	  from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT 
NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL 
THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES 
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE 
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE


*/

#include "prims/generatedPrimitives.hpp"
#include "oops/oop.inline.hpp"
#include "memory/universe.store.hpp"
#include "oops/memOop.inline.hpp"

// The interpreter's generated-primitive glue (call_primitive /
// call_primitive_can_fail followed by call_C) uses different conventions on
// the two backends:
//   x86:      receiver/argument live on the stack at [esp+8]/[esp+4], the
//             result is returned in eax, and ret(8) pops the two argument
//             slots.
//   AArch64:  AAPCS64 arguments arrive in x0 (receiver/self) and x1
//             (argument); the result must be left in x0 because call_C copies
//             x0 -> eax after the call; nothing was pushed, so ret(0).
#if defined(DELTA_ASSEMBLER_BACKEND_AARCH64)
#define PRIM_ARG_DECL()                                                                                                \
  Register argument = x1;                                                                                              \
  Register receiver = x0;
#define PRIM_RETURN()                                                                                                  \
  {                                                                                                                    \
    masm->mov(x0, eax);                                                                                                \
    masm->ret(0);                                                                                                      \
  }
#else
#define PRIM_ARG_DECL()                                                                                                \
  Address argument = Address(esp, 4);                                                                                  \
  Address receiver = Address(esp, 8);
#define PRIM_RETURN()                                                                                                  \
  { masm->ret(8); }
#endif

char* PrimitivesGenerator::smiOopPrimitives_add() {
  PRIM_ARG_DECL();
  Label _overflow;

  char* entry_point = masm->pc();

  masm->movl(eax, receiver);
  masm->addl(eax, argument);
  masm->jcc(Assembler::overflow, _overflow);
  masm->testb(eax, 0x03);
  masm->jcc(Assembler::notEqual, error_first_argument_has_wrong_type);
  PRIM_RETURN();

  masm->bind(_overflow);
  masm->movl(eax, argument);
  masm->testb(eax, 0x03);
  masm->jcc(Assembler::notEqual, error_first_argument_has_wrong_type);
  masm->jmp(error_overflow);

  return entry_point;
}

char* PrimitivesGenerator::smiOopPrimitives_subtract() {
  PRIM_ARG_DECL();
  Label _overflow;

  char* entry_point = masm->pc();

  masm->movl(eax, receiver);
  masm->subl(eax, argument);
  masm->jcc(Assembler::overflow, _overflow);
  masm->testb(eax, 0x03);
  masm->jcc(Assembler::notEqual, error_first_argument_has_wrong_type);
  PRIM_RETURN();

  masm->bind(_overflow);
  masm->movl(eax, argument);
  masm->testb(eax, 0x03);
  masm->jcc(Assembler::notEqual, error_first_argument_has_wrong_type);
  masm->jmp(error_overflow);

  return entry_point;
}

char* PrimitivesGenerator::smiOopPrimitives_multiply() {
  PRIM_ARG_DECL();
  Label _overflow;

  char* entry_point = masm->pc();

  masm->movl(edx, argument);
  masm->movl(eax, receiver);
  masm->testb(edx, 0x03);
  masm->jcc(Assembler::notEqual, error_first_argument_has_wrong_type);
  masm->sarl(edx, 2);
#if defined(DELTA_ASSEMBLER_BACKEND_AARCH64)
  // AArch64 imull: cmp sets EQ when no overflow, so test notEqual for overflow.
  masm->imull(edx);
  masm->jcc(Assembler::notEqual, _overflow);
#else
  masm->imull(edx);
  masm->jcc(Assembler::overflow, _overflow);
#endif
  masm->testb(eax, 0x03);
  masm->jcc(Assembler::notEqual, error_first_argument_has_wrong_type);
  PRIM_RETURN();

  masm->bind(_overflow);
  masm->movl(eax, argument);
  masm->testb(eax, 0x03);
  masm->jcc(Assembler::notEqual, error_first_argument_has_wrong_type);
  masm->jmp(error_overflow);

  return entry_point;
}

char* PrimitivesGenerator::smiOopPrimitives_mod() {
  PRIM_ARG_DECL();
  Label _equal, _positive;

  char* entry_point = masm->pc();

  // PUBLIC _smiOopPrimitives_mod@8
  //
  // ; Intel definition of mod delivers:
  // ;   0 <= |x%y| < |y|
  // ;
  // ; Standard definition requires:
  // ;   y>0:
  // ;     0 <= x mod y < y
  // ;   y<0:
  // ;     y <  x mod y <= 0
  // ;
  // ; Conversion:
  // ;
  // ;   sgn(y)=sgn(x%y):
  // ;     x mod y = x%y
  // ;
  // ;   sgn(y)#sgn(x%y):
  // ;     x mod y = x%y + y
  // ;

  //  masm->int3();
  masm->movl(eax, receiver);
  masm->movl(ecx, argument);
  masm->testl(ecx, ecx);
  masm->jcc(Assembler::equal, error_division_by_zero);

  masm->testb(ecx, 0x03);
  masm->jcc(Assembler::notEqual, error_first_argument_has_wrong_type);

  masm->sarl(ecx, 2);
  masm->sarl(eax, 2);
  masm->cdq();
  masm->idivl(ecx);
#if defined(DELTA_ASSEMBLER_BACKEND_AARCH64)
  masm->jcc(Assembler::notEqual, error_overflow);
#else
  masm->jcc(Assembler::overflow, error_overflow);
#endif

  masm->movl(eax, edx);
  masm->testl(eax, eax);
  masm->jcc(Assembler::equal, _equal);

  masm->xorl(edx, ecx);
  masm->jcc(Assembler::negative, _positive);

  masm->bind(_equal);
  masm->shll(eax, 2);
  PRIM_RETURN();

  masm->bind(_positive);
  masm->addl(eax, ecx);
  masm->shll(eax, 2);
  PRIM_RETURN();

  return entry_point;
}

char* PrimitivesGenerator::smiOopPrimitives_div() {
  PRIM_ARG_DECL();
  Label _equal, _positive;

  char* entry_point = masm->pc();

  // ; Intel definition of div delivers:
  // ;   x = (x/y)*y + (x%y)
  // ;
  // ; Standard definition requires:
  // ;   x = (x div y)*y + (x mod y)
  // ;
  // ; Conversion:
  // ;
  // ;   sgn(y)=sgn(x%y):
  // ;     x div y = x/y
  // ;
  // ;   sgn(y)#sgn(x%y):
  // ;     x div y = x/y-1
  // ;
  //

  masm->movl(ecx, argument);
  masm->movl(eax, receiver);
  masm->testl(ecx, ecx);
  masm->jcc(Assembler::equal, error_division_by_zero);

  masm->testb(ecx, 0x03);
  masm->jcc(Assembler::notEqual, error_first_argument_has_wrong_type);

  masm->sarl(ecx, 2);
  masm->sarl(eax, 2);
  masm->cdq();
  masm->idivl(ecx);

#if defined(DELTA_ASSEMBLER_BACKEND_AARCH64)
  masm->jcc(Assembler::notEqual, error_overflow);
#else
  masm->jcc(Assembler::overflow, error_overflow);
#endif

  masm->testl(edx, edx);
  masm->jcc(Assembler::equal, _equal);

  masm->xorl(ecx, edx);
  masm->jcc(Assembler::negative, _positive);

  masm->bind(_equal);
  masm->shll(eax, 2);
  PRIM_RETURN();

  masm->bind(_positive);
  masm->decl(eax);
  masm->shll(eax, 2);
  PRIM_RETURN();

  return entry_point;
}

char* PrimitivesGenerator::smiOopPrimitives_quo() {
  PRIM_ARG_DECL();

  char* entry_point = masm->pc();

  masm->movl(ecx, argument);
  masm->movl(eax, receiver);

  masm->testb(eax, 0x03);
  masm->jcc(Assembler::notEqual, error_receiver_has_wrong_type);

  masm->testl(ecx, ecx);
  masm->jcc(Assembler::equal, error_division_by_zero);

  masm->testb(ecx, 0x03);
  masm->jcc(Assembler::notEqual, error_first_argument_has_wrong_type);

  masm->sarl(ecx, 2);
  masm->sarl(eax, 2);
  masm->cdq();
  masm->idivl(ecx);

#if defined(DELTA_ASSEMBLER_BACKEND_AARCH64)
  masm->jcc(Assembler::notEqual, error_overflow);
#else
  masm->jcc(Assembler::overflow, error_overflow);
#endif
  masm->shll(eax, 2);
  PRIM_RETURN();

  return entry_point;
}

char* PrimitivesGenerator::smiOopPrimitives_remainder() {
  PRIM_ARG_DECL();

  char* entry_point = masm->pc();

  masm->movl(ecx, argument);
  masm->movl(eax, receiver);
  masm->testl(ecx, ecx);
  masm->jcc(Assembler::equal, error_division_by_zero);
  masm->testb(ecx, 0x03);
  masm->jcc(Assembler::notEqual, error_first_argument_has_wrong_type);
  masm->sarl(ecx, 2);
  masm->sarl(eax, 2);
  masm->cdq();
  masm->idivl(ecx);
#if defined(DELTA_ASSEMBLER_BACKEND_AARCH64)
  masm->jcc(Assembler::notEqual, error_overflow);
#else
  masm->jcc(Assembler::overflow, error_overflow);
#endif
  masm->movl(eax, edx);
  masm->sarl(eax, 2);
  PRIM_RETURN();

  return entry_point;
}
