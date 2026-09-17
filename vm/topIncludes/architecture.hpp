/*
Copyright (c) 2026, Gerardo Santana Gomez Garrido.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// Backend selection for the code generator.
//
// DELTA_BACKEND_AARCH64 / DELTA_BACKEND_X86_64 name the target architecture of
// the generated code (assembler, register mapping, instruction layouts,
// interpreter code generation). They are derived from the compiler's
// predefined macros, which describe the *target* (not the host), so they are
// always correct even when the target differs from the host (e.g. a forced
// `make ARCH=x86_64` build on Apple Silicon) and the root Makefile does not
// need to pass a backend define.
//
// An explicit -DDELTA_BACKEND_* is honored as an override before the
// builtin-derived fallback. It is intended only for builds that deliberately
// choose a backend other than the natural target (the standalone aarch64
// encoder test).

#ifndef _ARCHITECTURE_HPP
#define _ARCHITECTURE_HPP

#if defined(DELTA_BACKEND_AARCH64) && defined(DELTA_BACKEND_X86_64)
#error "only one backend selector may be defined: DELTA_BACKEND_AARCH64 or DELTA_BACKEND_X86_64, not both"
#elif defined(DELTA_BACKEND_AARCH64) || defined(DELTA_BACKEND_X86_64)
// explicit override selected on the command line
#elif defined(__aarch64__) || defined(_M_ARM64)
#define DELTA_BACKEND_AARCH64
#elif defined(__x86_64__) || defined(_M_X64)
#define DELTA_BACKEND_X86_64
#else
#error "unsupported backend: expected an aarch64 or x86-64 target"
#endif

#endif // _ARCHITECTURE_HPP