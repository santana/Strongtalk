/* Copyright 1994 - 1996, LongView Technologies L.L.C. $Revision: 1.37 $ */
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

#include "asm/abstractAssembler.hpp"
#include "asm/codeBuffer.hpp"
#include "memory/error.hpp"
#include "topIncludes/std_includes.hpp"
#include "utilities/ostream.hpp"

// Implementation of AbstractAssembler

AbstractAssembler::AbstractAssembler(CodeBuffer* code) {
  _code = code;
  _code_begin = code->code_begin();
  _code_limit = code->code_limit();
  _code_pos = code->code_end();
}

AbstractAssembler::~AbstractAssembler() {}

void AbstractAssembler::emit_byte(int x) {
  assert(isByte(x), "not a byte");
  *(unsigned char*)_code_pos = (unsigned char)x;
  _code_pos += sizeof(unsigned char);
  code()->set_code_end(_code_pos);
}

void AbstractAssembler::emit_long(int x) {
  *(int*)_code_pos = x;
  _code_pos += sizeof(int);
  code()->set_code_end(_code_pos);
}

void AbstractAssembler::emit_data(int data, relocInfo::relocType rtype) {
  if (rtype != relocInfo::none)
    code()->relocate(_code_pos, rtype);
  emit_long(data);
}

// Default label handling.
//
// The fixup of unbound label chains is backend-specific because it
// depends on the instruction encoding (the x86 backend patches relative
// displacements in the instruction stream, see X86Assembler). The
// defaults below are therefore conservative; concrete backends override
// bind/bind_to/link_to/print as needed.

void AbstractAssembler::bind_to(Label& L, int pos) {
  assert(0 <= pos && pos <= offset(), "must have a valid binding position");
  assert(!L.is_unbound(), "backend must resolve unbound label references");
  L.bind_to(pos);
}

void AbstractAssembler::link_to(Label& L, Label& appendix) {
  // Simple default: use appendix as the merged label if L is unused.
  if (L.is_unused() && appendix.is_unbound()) {
    L = appendix;
  }
  appendix.unuse(); // appendix should not be used anymore
}

void AbstractAssembler::bind(Label& L) {
  assert(!L.is_bound(), "label can only be bound once");
  bind_to(L, offset());
}

void AbstractAssembler::finalize() {
  if (_unbound_label.is_unbound()) {
    bind_to(_unbound_label, _binding_pos);
  }
}

void AbstractAssembler::print(Label& L) {
  if (L.is_unused()) {
    mystd->print_cr("undefined label");
  } else if (L.is_bound()) {
    mystd->print_cr("bound label to %d", L.pos());
  } else if (L.is_unbound()) {
    mystd->print_cr("unbound label (pos = %d)", L.pos());
  } else {
    mystd->print_cr("label in inconsistent state (pos = %d)", L._pos);
  }
}

void AbstractAssembler::merge(Label& L, Label& with) {
  Unimplemented();
}
