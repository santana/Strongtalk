/* Copyright 1994, 1995 LongView Technologies L.L.C. $Revision: 1.6 $ */
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

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "utilities/ostream.hpp"

#if defined(_MSC_VER)

#define int64_t __int64

#elif defined(__GNUC__)

#ifndef _WIN32 // mingw
#define _isnan(n) isnan(n)
#if defined(__APPLE__)
// Apple's math.h has no finite(); use the C99 isfinite()
#define _finite(n) isfinite(n)
#else
#define _finite(n) finite(n)
#endif
#endif

#define _vsnprintf vsnprintf

// __stdcall is a real calling convention only on 32-bit x86; on 64-bit
// targets (x86-64, AArch64) the attribute is meaningless and clang flags it
// with -Wignored-attributes. Leave it empty so the primitive/trampoline
// declarations below don't emit noise on the ports this VM actually builds.
#if defined(__i386__) || defined(_M_IX86)
#define __stdcall __attribute__((stdcall))
#else
#define __stdcall
#endif
#define mystd _mystd

#else

#error Unrecognized compiler

#endif
