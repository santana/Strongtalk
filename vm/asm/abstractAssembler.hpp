/*
Copyright (c) 2026, Gerardo Santana Gomez Garrido.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// The architecture-independent part of the assembler.
//
// Concrete assembler backends (e.g. X86Assembler, see assembler_x86.hpp)
// extend AbstractAssembler and implement the instruction set plus the
// backend-specific label fixup logic (bind_to/link_to). All code that
// wants to generate machine code should go through the platform selected
// by "asm/assembler.hpp" and not reference AbstractAssembler directly.

#ifndef _ABSTRACT_ASSEMBLER_HPP
#define _ABSTRACT_ASSEMBLER_HPP

#include "code/relocInfo.hpp"
#include "memory/allocation.hpp"

class CodeBuffer;

// Label represent a target destination for jumps, calls and non-local returns.
// After declaration they can be freely used to denote known or (yet) unknown
// target destinations. Assembler::bind is used to bind a label to the current
// code position. A label can be bound only once.

class Label : public ValueObj {
 private:
  // _pos encodes both the binding state (via its sign)
  // and the binding position (via its value) of a label.
  //
  // _pos <  0	bound label, pos() returns the target (jump) position
  // _pos == 0	unused label
  // _pos >  0	unbound label, pos() returns the last displacement (see .cpp file) in the chain
  int _pos;

  int pos() const {
    if (_pos < 0) return -_pos - 1;
    if (_pos > 0) return  _pos - 1;
    ShouldNotReachHere();
    return 0;
  }

  void bind_to(int pos)		{ assert(pos >= 0, "illegal position"); _pos = -pos - 1; }
  void link_to(int pos)		{ assert(pos >= 0, "illegal position"); _pos =  pos + 1; }
  void unuse()			{ _pos = 0; }

 public:
  bool is_bound() const		{ return _pos <  0; }
  bool is_unbound() const	{ return _pos >  0; }
  bool is_unused() const	{ return _pos == 0; }

  Label() : _pos(0)		{}
  ~Label()			{ assert(!is_unbound(), "unbound label"); }

  friend class AbstractAssembler;
#if defined(DELTA_ASSEMBLER_BACKEND_AARCH64)
  friend class AArch64Assembler;
  friend class AArch64MacroAssembler;
  friend class AArch64Displacement;
#else
  friend class X86Assembler;
  friend class X86MacroAssembler;
  friend class Displacement;
#endif
};


// AbstractAssembler provides the backend-independent assembler
// infrastructure: the code buffer, byte emission primitives and label
// handling. Label fixup (bind_to/link_to/bind/finalize/print) is
// backend-specific because it depends on the instruction encoding and
// is therefore virtual and overridden by concrete backends.

class AbstractAssembler: public ResourceObj {
 protected:
  CodeBuffer* _code;

  char* _code_begin;			// first byte of code buffer
  char* _code_limit;			// first byte after code buffer
  char* _code_pos;			// current code generation position

  Label _unbound_label;			// the last label to be bound to _binding_pos, if unbound
  int	_binding_pos;			// the position to which _unbound_label has to be bound, if there

  char* addr_at(int pos) 		{ return _code_begin + pos; }

  int  byte_at(int pos)			{ return *(unsigned char*)addr_at(pos); }
  void byte_at_put(int pos, int x)	{ *(unsigned char*)addr_at(pos) = (unsigned char)x; }

  int  long_at(int pos)			{ return *(int*)addr_at(pos); }
  void long_at_put(int pos, int x)	{ *(int*)addr_at(pos) = x; }

  bool is8bit(int x)			{ return -0x80 <= x && x < 0x80; }
  bool isByte(int x)			{ return 0 <= x && x < 0x100; }
  bool isShiftCount(int x)		{ return 0 <= x && x < 32; }

  void emit_byte(int x);
  void emit_long(int x);
  void emit_data(int data, relocInfo::relocType rtype);

  // Label fixup - backend specific.
  virtual void print  (Label& L);
  virtual void bind_to(Label& L, int pos);
  virtual void link_to(Label& L, Label& appendix);

 public:
  AbstractAssembler(CodeBuffer* code);
  ~AbstractAssembler();

  void		finalize();		// call this before using/copying the code
  CodeBuffer*	code() const		{ return _code; }
  char*		pc() const		{ return _code_pos; }
  int		offset() const		{ return _code_pos - _code_begin; }

  // Labels
  virtual void bind(Label& L);		// binds an unbound label L to the current code position
  virtual void merge(Label& L, Label& with);	// merges L and with, L is the merged label
};
#endif // _ABSTRACT_ASSEMBLER_HPP
