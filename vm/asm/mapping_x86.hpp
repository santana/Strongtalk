/*
Copyright (c) 2026, Gerardo Santana Gomez Garrido.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// The x86/x86-64 mapping backend.  It provides the architecture-specific
// constants that the Mapping interface in "asm/mapping.hpp" depends on
// (register counts, frame offsets and the register conventions).  It is
// included by "asm/mapping.hpp"; do not include it directly.
//
// On x86-64 the System V AMD64 ABI passes the first 6 integer args in
// rdi, rsi, rdx, rcx, r8, r9, but the Delta interpreter/compiler
// convention pushes all arguments on the stack (same as x86-32), so
// nofArgRegisters is 0.
//
// The x87 FPU stack is used for floating-point (UseFPUStack = true);
// there is no FloatRegister class, so float_scratch_reg is a dummy
// integer register that is never referenced at runtime.

#ifndef _MAPPING_X86_HPP
#define _MAPPING_X86_HPP

// Register usage
const int nofArgRegisters	= 0;			// Delta args are passed on the stack
const int nofLocalRegisters	= 3;			// max. number of temporaries allocated in registers


// Temporaries on the stack
const int first_temp_offset	= -1;			// offset of first temporary relative to rbp (in oops)
const int first_float_offset	= -4;			// offset of first float relative to 8-byte aligned rbp


// calls
const Register self_reg		= eax;			// incoming receiver location (in prologue of callee)
const Register receiver_reg	= eax;			// outgoing receiver location (before call)
const Register result_reg	= eax;			// outgoing result location (before exit)
const Register frame_reg	= ebp;			// activation frame pointer


// non-local returns
const Register NLR_result_reg	= eax;			// result being returned
const Register NLR_home_reg	= edi;			// frame ptr of home frame (stack)
const Register NLR_homeId_reg	= esi;			// scope id of home scope (inlining)


// temporaries for local code generation (within one Node only)
// note: these locations must not intersect with any location used
// for non-local returns!
#if DELTA_X86_64
// r8-r10 are caller-saved scratch; r12-r14 are callee-saved locals.
const Register temp1		= r8;
const Register temp2		= r9;
const Register temp3		= r10;
// Local registers (callee-saved) are defined separately in Mapping::initialize();
// they must be disjoint from temp1..temp3.
const Register localReg0	= r12;
const Register localReg1	= r13;
const Register localReg2	= r14;
#else
const Register temp1		= ecx;
const Register temp2		= edx;
const Register temp3		= ecx;
const Register localReg0	= ebx;
const Register localReg1	= esi;
const Register localReg2	= edi;
#endif


// The x87 FPU stack is used for floating-point on x86 (UseFPUStack = true),
// so fload/fstore never need a scratch GP register.  This constant exists
// only to satisfy the Mapping interface; it is never loaded or stored.
const Register float_scratch_reg = ecx;


#endif // _MAPPING_X86_HPP
