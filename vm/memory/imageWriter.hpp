/*
Copyright (c) 2026, Gerardo Santana Gomez Garrido.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
following conditions are met:

1. Redistributions of source code must retain the above copyright notice and this list of conditions and the
   following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice and this list of conditions and the
   following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION, HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE
*/

// Native 64-bit image writer (Plan A Phase 5).
//
// Dumps the running 64-bit heap to a bootstrap-text image (the tracked
// strongtalk.bst, or any file passed to -SaveImage) that
// vm/runtime/bootstrap.cpp can consume. The stream starts with the version
// integer 200 + Bytecodes::version(), followed by the 26 bootstrap roots.
// Objects are emitted inline depth-first: the first reference writes the
// object in full, later references are '3' + index. An object's index is
// assigned when its header is emitted -- the same moment the reader allocates
// and registers it -- so repeated references, self references, and cycles
// always resolve. Sizes are native oop counts and method codes are written in
// the native layout; the reader applies no 32-bit fixups.
//
// Kept after Phase 6 so the image can be regenerated from a running VM.

#ifndef _IMAGEWRITER_HPP
#define _IMAGEWRITER_HPP

#include "oops/oop.hpp"

// Emit the tenured heap as a bootstrap-text image at `path`.  Returns the
// number of objects written, or -1 on error.  Intended to be called once the
// VM has booted (after load_image), before the scheduler starts, or on demand
// via the -SaveImage VM flag.
int image_writer_save(const char* path);

#endif // _IMAGEWRITER_HPP
