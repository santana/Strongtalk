/*
Copyright (c) 2026, Gerardo Santana Gomez Garrido.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// The AArch64 (ARM64) mapping backend. It provides the architecture-specific
// constants that the Mapping interface in "asm/mapping.hpp" depends on
// (register counts, frame offsets and the register conventions). It is
// included by "asm/mapping.hpp"; do not include it directly.
//
// Conventions (AAPCS64, with the strongtalk registers layered on top):
//   - the first 8 arguments are passed in x0..x7 (x0 holds the receiver);
//   - x29 is the frame pointer, x30 the link register;
//   - x19/x20 carry the non-local-return home frame and scope id, so they are
//     callee-saved and must not be clobbered by compiled code;
//   - x21..x23 are the "local registers" (callee-saved temporaries);
//   - x9..x11 are scratch temporaries for code generation;
//   - there is no FPU stack: floats live in memory or in the vector registers,
//     and fload/fstore materialize values through the scratch register d31.
//
// The stack offsets are a first proposal and must be validated together with
// the interpreter/compiler prologue and epilogue during the JIT retarget
// phase.

#ifndef _MAPPING_AARCH64_HPP
#define _MAPPING_AARCH64_HPP

// Register usage
const int nofArgRegisters	= 8;			// max. number of arguments (excl. receiver) passed in registers
const int nofLocalRegisters	= 3;			// max. number of temporaries allocated in registers


// Temporaries on the stack
const int first_temp_offset	= -1;			// offset of first temporary relative to x29 if there are no floats
const int first_float_offset	= -4;			// offset of first float relative to 8byte aligned x29 value (= base)


// calls
const Register self_reg		= x0;			// incoming receiver location (in prologue of callee)
const Register receiver_reg	= x0;			// outgoing receiver location (before call)
const Register result_reg	= x0;			// outgoing result location (before exit)
const Register frame_reg	= x29;			// activation frame pointer


// non-local returns
// These must match the registers used by the interpreter's non-local return
// code (see InterpreterGenerator::generate_nonlocal_return_code), which are
// the x86-compatible names eax/edi/esi. Like on x86, they are caller-saved:
// compiled code reloads them after calls into C as needed.
const Register NLR_result_reg	= eax;			// result being returned
const Register NLR_home_reg	= edi;			// frame ptr of home frame (stack)
const Register NLR_homeId_reg	= esi;			// scope id of home scope (inlining)


// temporaries for local code generation (within one Node only)
// note: these locations must not intersect with any location used
// for non-local returns!
const Register temp1		= x9;
const Register temp2		= x10;
const Register temp3		= x11;


// scratch vector register used by fload/fstore (no FPU stack on AArch64)
const FloatRegister float_scratch_reg = d31;


#endif // _MAPPING_AARCH64_HPP
