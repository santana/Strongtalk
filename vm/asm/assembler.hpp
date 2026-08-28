/* Copyright 1994 - 1996, LongView Technologies L.L.C. $Revision: 1.34 $ */
/* Copyright (c) 2006, Sun Microsystems, Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the 
following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following 
	  disclaimer in the documentation and/or other materials provided with the distribution.
    * Neither the name of Sun Microsystems nor the names of its contributors may be used to endorse or promote products derived 
	  from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT 
NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL 
THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES 
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE 
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE


*/

// This is the single entry point for the assembler. It is split into a
// backend-independent part (asm/abstractAssembler.hpp) and one implementation
// per target architecture (asm/assembler_<arch>.hpp). The concrete backend
// is aliased to Assembler / MacroAssembler below, so the rest of the VM can
// be written in an architecture-neutral way.
//
// The default backend is x86. To build for another backend, define the
// corresponding DELTA_ASSEMBLER_BACKEND_* macro and add an implementation
// under vm/asm/ following the pattern of the x86 one.

#ifndef _ASSEMBLER_HPP
#define _ASSEMBLER_HPP

#include "asm/abstractAssembler.hpp"

#if defined(DELTA_ASSEMBLER_BACKEND_AARCH64)
#include "asm/assembler_aarch64.hpp"
typedef AArch64Assembler Assembler;
typedef AArch64MacroAssembler MacroAssembler;
#else
// default backend: x86
#include "asm/assembler_x86.hpp"
typedef X86Assembler Assembler;
typedef X86MacroAssembler MacroAssembler;
#endif

extern MacroAssembler* theMacroAssm;

#endif // _ASSEMBLER_HPP
