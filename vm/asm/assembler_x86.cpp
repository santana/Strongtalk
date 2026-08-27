/*
Copyright (c) 2026, Gerardo Santana Gomez Garrido.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#if defined(DELTA_ASSEMBLER_BACKEND_AARCH64)
// The AArch64 backend has its own assembler (asm/assembler_aarch64.cpp); this
// file implements the x86 encoder and must not be compiled on AArch64.
#else

#include "asm/assembler.hpp"
#include "asm/codeBuffer.hpp"
#include "code/nativeInstruction.hpp"
#include "code/stubRoutines.hpp"
#include "memory/universe.hpp"
#include "memory/error.hpp"
#include "memory/util.hpp"
#include "oops/oop.hpp"
#include "oops/smiOop.hpp"
#include "runtime/debug.hpp"
#include "runtime/process.hpp"
#include "runtime/runtime.hpp"
#include "topIncludes/std_includes.hpp"
#include "utilities/ostream.hpp"

#include <cstdint>

// A Displacement describes the 32bit immediate field of an instruction which
// may be used together with a Label in order to refer to a yet unknown code
// position. Displacements stored in the instruction stream are used to describe
// the instruction and to chain a list of instructions using the same Label.
// A Displacement contains 3 different fields:
//
// next field: position of next displacement in the chain (0 = end of list)
// type field: instruction type
// info field: instruction specific information
//
// A next value of null (0) indicates the end of a chain (note that there can
// be no displacement at position zero, because there is always at least one
// instruction byte before the displacement).
//
// Displacement _data field layout
//
// |31....10|9......8|7......0|
// [  next  |  type  |  info  ]

class Displacement : public ValueObj {
 private:
  int _data;

  enum Layout {
    info_size	=  IC_Info::number_of_flags,
    type_size	=  2,
    next_size	= 32 - (type_size + info_size),

    info_pos	= 0,
    type_pos	= info_pos + info_size,
    next_pos	= type_pos + type_size,

    info_mask	= (1 << info_size) - 1,
    type_mask	= (1 << type_size) - 1,
    next_mask	= (1 << next_size) - 1,
  };

  enum Type {			// info field usage
    call,			// unused
    absolute_jump,		// unused
    conditional_jump,		// condition code
    ic_info,			// flags
  };

  void init(Label& L, Type type, int info) {
    assert(!L.is_bound(), "label is bound");
    int next = 0;
    if (L.is_unbound()) {
      next = L.pos();
      assert(next > 0, "Displacements must be at positions > 0");
    }
    assert((next & ~next_mask) == 0, "next field too small");
    assert((type & ~type_mask) == 0, "type field too small");
    assert((info & ~info_mask) == 0, "info field too small");
    _data = (next << next_pos) | (type << type_pos) | (info << info_pos);
  }

  int  data() const		{ return _data; }
  int  info() const		{ return     ((_data >> info_pos) & info_mask); }
  Type type() const		{ return Type((_data >> type_pos) & type_mask); }
  void next(Label & L) const	{ int n    = ((_data >> next_pos) & next_mask); n > 0 ? L.link_to(n) : L.unuse(); }
  void link_to(Label& L)	{ init(L, type(), info()); }

  Displacement(int data)	{ _data = data; }

  Displacement(Label& L, Type type, int info) {
    init(L, type, info);
  }

  void print() {
    char* s;
    switch (type()) {
      case call            : s = "call"; break;
      case absolute_jump   : s = "jmp "; break;
      case conditional_jump: s = "jcc "; break;
      case ic_info         : s = "nlr "; break;
      default              : s = "????"; break;
    }
    mystd->print("%s (info = 0x%x)", s, info());
  }

  friend class X86Assembler;
  friend class X86MacroAssembler;
};


// Use macros (otherwise must also declare Displacement class in .hpp file)
#define disp_at(L)		Displacement(long_at((L).pos()))
#define disp_at_put(L,disp)	long_at_put((L).pos(), (disp).data())
#define emit_disp(L,type,info)	{ Displacement disp((L), (type), (info));	\
				  L.link_to(offset());				\
				  emit_long(int(disp.data()));			\
				}

// Implementation of Register
#if DELTA_X86_64
const char* registerNames[nofRegisters] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
					   "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
#else
const char* registerNames[nofRegisters] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
#endif

char* Register::name() const {
  return (char*) (isValid() ? registerNames[_number] : "noreg");
}


// Implementation of Address

Address::Address() {
  _base  = noreg;
  _index = noreg;
  _scale = no_scale;
  _disp  = 0;
  _rtype = relocInfo::none;
}


Address::Address(intptr_t disp, relocInfo::relocType rtype) {
  _base  = noreg;
  _index = noreg;
  _scale = no_scale;
  _disp  = disp;
  _rtype = rtype;
}


Address::Address(Register base, intptr_t disp, relocInfo::relocType rtype) {
  _base  = base;
  _index = noreg;
  _scale = no_scale;
  _disp  = disp;
  _rtype = rtype;
}


Address::Address(Register base, Register index, ScaleFactor scale, intptr_t disp, relocInfo::relocType rtype) {
  assert((index == noreg) == (scale == Address::no_scale), "inconsistent address");
  _base  = base;
  _index = index;
  _scale = scale;
  _disp  = disp;
  _rtype = rtype;
}


// Implementation of X86Assembler

X86Assembler::X86Assembler(CodeBuffer* code)
  : AbstractAssembler(code)
{
  _last_rex = 0;
}


// REX prefix support

int X86Assembler::rex_bits(Register reg, Register base, Register index) {
  int rex = 0;
  if (DELTA_X86_64) {
    if (reg.isValid()   && (reg.number()   & 8)) rex |= 0x04;	// REX.R
    if (index.isValid() && (index.number() & 8)) rex |= 0x02;	// REX.X
    if (base.isValid()  && (base.number()  & 8)) rex |= 0x01;	// REX.B
  }
  return rex;
}


void X86Assembler::emit_rex(int rex) {
  if (rex != 0) {
    emit_byte(0x40 | (rex & 0x0F));
    _last_rex = rex;
  }
}


void X86Assembler::emit_rex_w(Register reg, Register base, Register index) {
  emit_rex(0x08 | rex_bits(reg, base, index));	// REX.W + operand bits
}


void X86Assembler::emit_quad_data(intptr_t data, relocInfo::relocType rtype) {
  if (rtype != relocInfo::none) code()->relocate(_code_pos, rtype);
  emit_long((int)(data & 0xFFFFFFFF));
  emit_long((int)((data >> 32) & 0xFFFFFFFF));
}


void X86Assembler::emit_arith_b(int op1, int op2, Register dst, int imm8) {
  guarantee(dst.hasByteRegister(), "must have byte register");
  assert(isByte(op1) && isByte(op2), "wrong opcode");
  assert(isByte(imm8), "not a byte");
  assert((op1 & 0x01) == 0, "should be 8bit operation");
  emit_byte(op1);
  emit_byte(op2 | dst.number());
  emit_byte(imm8);
}


void X86Assembler::emit_arith(int op1, int op2, Register dst, int imm32, bool rex_w) {
  assert(isByte(op1) && isByte(op2), "wrong opcode");
  assert((op1 & 0x01) == 1, "should be 32bit operation");
  assert((op1 & 0x02) == 0, "sign-extension bit should not be set");
  emit_rex(rex_bits(dst) | (rex_w ? 0x08 : 0));
  if (is8bit(imm32)) {
    emit_byte(op1 | 0x02); // set sign bit
    emit_byte(op2 | (dst.number() & 7));
    emit_byte(imm32 & 0xFF);
  } else {
    emit_byte(op1);
    emit_byte(op2 | (dst.number() & 7));
    emit_long(imm32);
  }
}


// Note: a 32-bit immediate can only hold a 32-bit oop. On a 64-bit build an
// oop must be materialized in a register first (movq) - see the 64-bit port
// notes in the header.
void X86Assembler::emit_arith(int op1, int op2, Register dst, oop obj) {
  assert(isByte(op1) && isByte(op2), "wrong opcode");
  assert((op1 & 0x01) == 1, "should be 32bit operation");
  assert((op1 & 0x02) == 0, "sign-extension bit should not be set");
  assert(BytesPerNativeWord == 4, "oop immediate does not fit into a 32-bit instruction on 64-bit - materialize the oop in a register (movq) first");
  emit_byte(op1);
  emit_byte(op2 | dst.number());
  emit_data((intptr_t)obj, relocInfo::oop_type);
}


void X86Assembler::emit_arith(int op1, int op2, Register dst, Register src, bool rex_w) {
  assert(isByte(op1) && isByte(op2), "wrong opcode");
  emit_rex(rex_bits(dst, src) | (rex_w ? 0x08 : 0));
  emit_byte(op1);
  emit_byte(op2 | (dst.number() & 7) << 3 | (src.number() & 7));
}


void X86Assembler::emit_operand(Register reg, Register base, Register index, Address::ScaleFactor scale, intptr_t disp, relocInfo::relocType rtype) {
  // The REX prefix for the operand registers (REX.R/X/B) must already have
  // been emitted by the caller before the opcode (see emit_rex). The ModRM
  // fields below carry only the low 3 register bits.
  assert((rex_bits(reg, base, index) & ~_last_rex) == 0, "missing REX prefix before opcode");
  const int r  = reg.number()   & 7;
  if (base.isValid()) {
    const int b  = base.number()  & 7;
    if (index.isValid()) {
      const int ix = index.number() & 7;
      assert(scale != Address::no_scale, "inconsistent address");
      // [base + index*scale + disp]
      assert(index != esp, "illegal addressing mode");
      if (disp == 0 && rtype == relocInfo::none && b != 5) {
        // [base + index*scale]
        // [00 reg 100][ss index base]
        emit_byte(0x04 | r << 3);
        emit_byte(scale << 6 | ix << 3 | b);
      } else if (is8bit(disp) && rtype == relocInfo::none) {
        // [base + index*scale + imm8]
        // [01 reg 100][ss index base] imm8
        emit_byte(0x44 | r << 3);
        emit_byte(scale << 6 | ix << 3 | b);
        emit_byte(disp & 0xFF);
      } else {
        // [base + index*scale + imm32]
        // [10 reg 100][ss index base] imm32
        emit_byte(0x84 | r << 3);
        emit_byte(scale << 6 | ix << 3 | b);
        emit_data(disp, rtype);
      }
    } else if (b == 4) {
      // [esp + disp] (and [r12 + disp] on 64-bit) - always needs a SIB byte
      if (disp == 0 && rtype == relocInfo::none) {
        // [esp]
        // [00 reg 100][00 100 100]
        emit_byte(0x04 | r << 3);
        emit_byte(0x24);
      } else if (is8bit(disp) && rtype == relocInfo::none) {
        // [esp + imm8]
        // [01 reg 100][00 100 100] imm8
        emit_byte(0x44 | r << 3);
        emit_byte(0x24);
        emit_byte(disp & 0xFF);
      } else {
        // [esp + imm32]
        // [10 reg 100][00 100 100] imm32
        emit_byte(0x84 | r << 3);
        emit_byte(0x24);
        emit_data(disp, rtype);
      }
    } else {
      // [base + disp]
      // Note: a base with a low-3-bit value of 5 (ebp/r13) can never use the
      // mod=00 (no displacement) form - with mod=00 the r/m=101 field encodes
      // disp32-only / RIP-relative, so such bases need at least a disp8.
      if (disp == 0 && rtype == relocInfo::none && b != 5) {
        // [base]
        // [00 reg base]
        emit_byte(0x00 | r << 3 | b);
      } else if (is8bit(disp) && rtype == relocInfo::none) {
        // [base + imm8]
        // [01 reg base] imm8
        emit_byte(0x40 | r << 3 | b);
        emit_byte(disp & 0xFF);
      } else {
        // [base + imm32]
        // [10 reg base] imm32
        emit_byte(0x80 | r << 3 | b);
        emit_data(disp, rtype);
      }
    }
  } else {
    if (index.isValid()) {
      const int ix = index.number() & 7;
      assert(scale != Address::no_scale, "inconsistent address");
      // [index*scale + disp]
      // [00 reg 100][ss index 101] imm32
      assert(index != esp, "illegal addressing mode");
      emit_byte(0x04 | r << 3);
      emit_byte(scale << 6 | ix << 3 | 0x05);
      if (DELTA_X86_64) {
        // WARNING: on x86-64 this form is nominally [index*scale + RIP-relative
        // disp32], but the Rosetta 2 emulator decodes it as ABSOLUTE disp32.
        // Do NOT use this addressing form in 64-bit code; load the base via the
        // [rip+disp] form into a register and use [base + index*scale] instead.
        assert(false, "no-base indexed addressing is unusable on x86-64 (Rosetta)");
        emit_data(disp, rtype);
      } else {
        // On 32-bit this is absolute addressing.
        emit_data(disp, rtype);
      }
    } else {
      // [disp]
      // [00 reg 101] imm32
      if (DELTA_X86_64 && rtype != relocInfo::none) {
        // On x86-64 mod=00 r/m=101 is RIP-relative addressing, so an
        // absolute address (e.g. &last_Delta_fp) cannot be encoded directly.
        // Emit the displacement relative to the end of the disp32 field; the
        // recorded relocation keeps the reference fixable when the code moves
        // (see nmethod::fix_relocation_at_move, which adds the move delta).
        emit_byte(0x05 | r << 3);
        code()->relocate(pc(), rtype);
        // RIP-relative disp32: next_pc is the end of the disp field, i.e. the
        // address of the next instruction.
        intptr_t next_pc = (intptr_t) pc() + 4;
        emit_long((int) (disp - next_pc));
      } else {
        // On 32-bit mod=00 r/m=101 is absolute addressing; on 64-bit a
        // non-relocated disp32 is a RIP-relative offset.
        emit_byte(0x05 | r << 3);
        emit_data(disp, rtype);
      }
    }
  }
}


void X86Assembler::emit_operand(Register reg, Address adr) {
  emit_operand(reg, adr._base, adr._index, adr._scale, adr._disp, adr._rtype);
}


void X86Assembler::emit_farith(int b1, int b2, int i) {
  assert(isByte(b1) && isByte(b2), "wrong opcode");
  assert(0 <= i &&  i < 8, "illegal stack offset");
  emit_byte(b1);
  emit_byte(b2 + i);
}


void X86Assembler::pushad() {
  // 0x60 is an invalid opcode in 64-bit mode; there is no 64-bit pushad.
  // Save the same general registers (all except esp) with individual pushes.
  if (BytesPerNativeWord == 8) {
    pushq(eax);
    pushq(ecx);
    pushq(edx);
    pushq(ebx);
    pushq(ebp);
    pushq(esi);
    pushq(edi);
  } else {
    emit_byte(0x60);
  }
}


void X86Assembler::popad() {
  // 0x61 is an invalid opcode in 64-bit mode; there is no 64-bit popad.
  // Restore in the reverse order of pushad.
  if (BytesPerNativeWord == 8) {
    popq(edi);
    popq(esi);
    popq(ebp);
    popq(ebx);
    popq(edx);
    popq(ecx);
    popq(eax);
  } else {
    emit_byte(0x61);
  }
}


void X86Assembler::pushl(int imm32) {
  // On a 64-bit build this pushes the sign-extended 64-bit value (there is
  // no 32-bit push in 64-bit mode), so the stack effect is always 8 bytes.
  emit_byte(0x68);
  emit_long(imm32);
}


void X86Assembler::pushl(oop obj) {
  assert(BytesPerNativeWord == 4, "an oop does not fit into a push imm32 on 64-bit - materialize the oop in a register (movq) first");
  emit_byte(0x68);
  emit_data((intptr_t)obj, relocInfo::oop_type);
}


void X86Assembler::pushl(Register src) {
  emit_rex(rex_bits(noreg, src, noreg));	// REX.B for r8..r15
  emit_byte(0x50 | (src.number() & 7));
}


void X86Assembler::pushl(Address src) {
  emit_rex(rex_bits(noreg, src._base, src._index));
  emit_byte(0xFF);
  emit_operand(esi, src);
}


void X86Assembler::popl(Register dst) {
  emit_rex(rex_bits(noreg, dst, noreg));	// REX.B for r8..r15
  emit_byte(0x58 | (dst.number() & 7));
}


void X86Assembler::popl(Address dst) {
  emit_rex(rex_bits(noreg, dst._base, dst._index));
  emit_byte(0x8F);
  emit_operand(eax, dst);
}


// 64-bit stack operations (x86-64)

void X86Assembler::pushq(Register src) {
  assert(BytesPerNativeWord == 8, "pushq is only available on 64-bit");
  emit_rex_w(noreg, src, noreg);
  emit_byte(0x50 | (src.number() & 7));
}


void X86Assembler::popq(Register dst) {
  assert(BytesPerNativeWord == 8, "popq is only available on 64-bit");
  emit_rex_w(noreg, dst, noreg);
  emit_byte(0x58 | (dst.number() & 7));
}


void X86Assembler::movb(Register dst, Address src) {
  guarantee(dst.hasByteRegister(), "must have byte register");
  emit_byte(0x8A);
  emit_operand(dst, src);
}


void X86Assembler::movb(Address dst, int imm8) {
  emit_byte(0xC6);
  emit_operand(eax, dst);
  emit_byte(imm8);
}


void X86Assembler::movb(Address dst, Register src) {
  guarantee(src.hasByteRegister(), "must have byte register");
  emit_byte(0x88);
  emit_operand(src, dst);
}


void X86Assembler::movw(Register dst, Address src) {
  emit_byte(0x66);
  emit_byte(0x8B);
  emit_operand(dst, src);
}


void X86Assembler::movw(Address dst, Register src) {
  emit_byte(0x66);
  emit_byte(0x89);
  emit_operand(src, dst);
}


void X86Assembler::movl(Register dst, int imm32) {
  emit_rex(rex_bits(noreg, dst, noreg));	// REX.B for r8d..r15d
  emit_byte(0xB8 | (dst.number() & 7));
  emit_long(imm32);
}


void X86Assembler::movl(Register dst, oop obj) {
  if (DELTA_X86_64) {
    // On a 64-bit build an oop is a full 64-bit value; load it with a
    // 64-bit immediate (movabs) instead of truncating it to 32 bits.
    emit_rex_w(noreg, dst, noreg);
    emit_byte(0xB8 | (dst.number() & 7));
    emit_quad_data((intptr_t)obj, relocInfo::oop_type);
  } else {
    emit_byte(0xB8 | dst.number());
    emit_data((intptr_t)obj, relocInfo::oop_type);
  }
}


void X86Assembler::movl(Register dst, Register src) {
  emit_rex(rex_bits(dst, src));
  emit_byte(0x8B);
  emit_byte(0xC0 | (dst.number() & 7) << 3 | (src.number() & 7));
}


void X86Assembler::movl(Register dst, Address src) {
  emit_rex(rex_bits(dst, src._base, src._index));
  emit_byte(0x8B);
  emit_operand(dst, src);
}


void X86Assembler::movl(Address dst, int imm32) {
  emit_rex(rex_bits(noreg, dst._base, dst._index));
  emit_byte(0xC7);
  emit_operand(eax, dst);
  emit_long(imm32);
}


void X86Assembler::movl(Address dst, oop obj) {
  // On a 64-bit build a store of a full 64-bit oop to memory cannot be
  // encoded without a scratch register - materialize the oop in a register
  // (movq) first and store that.
  assert(BytesPerNativeWord == 4, "an oop does not fit into a mov r/m32, imm32 on 64-bit - materialize the oop in a register (movq) first");
  emit_byte(0xC7);
  emit_operand(eax, dst);
  emit_data((intptr_t)obj, relocInfo::oop_type);
}


void X86Assembler::movl(Address dst, Register src) {
  emit_rex(rex_bits(src, dst._base, dst._index));
  emit_byte(0x89);
  emit_operand(src, dst);
}


// 64-bit moves (x86-64)

void X86Assembler::movq(Register dst, Register src) {
  assert(BytesPerNativeWord == 8, "movq is only available on 64-bit");
  emit_rex_w(dst, src);
  emit_byte(0x8B);
  emit_byte(0xC0 | (dst.number() & 7) << 3 | (src.number() & 7));
}


void X86Assembler::movq(Register dst, Address src) {
  assert(BytesPerNativeWord == 8, "movq is only available on 64-bit");
  emit_rex_w(dst, src._base, src._index);
  emit_byte(0x8B);
  emit_operand(dst, src);
}


void X86Assembler::movq(Address dst, Register src) {
  assert(BytesPerNativeWord == 8, "movq is only available on 64-bit");
  emit_rex_w(src, dst._base, dst._index);
  emit_byte(0x89);
  emit_operand(src, dst);
}


void X86Assembler::movq(Address dst, int imm32) {
  assert(BytesPerNativeWord == 8, "movq is only available on 64-bit");
  emit_rex_w(noreg, dst._base, dst._index);
  emit_byte(0xC7);
  emit_operand(eax, dst);
  emit_long(imm32);
}


void X86Assembler::movq(Register dst, intptr_t imm) {
  assert(BytesPerNativeWord == 8, "movq is only available on 64-bit");
  emit_rex_w(noreg, dst, noreg);
  emit_byte(0xB8 | (dst.number() & 7));
  emit_quad_data(imm, relocInfo::none);
}


void X86Assembler::movq(Register dst, oop obj) {
  assert(BytesPerNativeWord == 8, "movq is only available on 64-bit");
  emit_rex_w(noreg, dst, noreg);
  emit_byte(0xB8 | (dst.number() & 7));
  emit_quad_data((intptr_t)obj, relocInfo::oop_type);
}


void X86Assembler::movsxq(Register dst, Register src) {
  assert(BytesPerNativeWord == 8, "movsxq is only available on 64-bit");
  emit_rex_w(dst, src);		// movsxd r64, r/m32
  emit_byte(0x63);
  emit_byte(0xC0 | (dst.number() & 7) << 3 | (src.number() & 7));
}


void X86Assembler::movsxb(Register dst, Address src) {
  emit_rex(rex_bits(dst, src._base, src._index));
  emit_byte(0x0F);
  emit_byte(0xBE);
  emit_operand(dst, src);
}


void X86Assembler::movsxb(Register dst, Register src) {
  guarantee(src.hasByteRegister(), "must have byte register");
  emit_rex(rex_bits(dst, src));
  emit_byte(0x0F);
  emit_byte(0xBE);
  emit_byte(0xC0 | (dst.number() & 7) << 3 | (src.number() & 7));
}


void X86Assembler::movsxw(Register dst, Address src) {
  emit_rex(rex_bits(dst, src._base, src._index));
  emit_byte(0x0F);
  emit_byte(0xBF);
  emit_operand(dst, src);
}


void X86Assembler::movsxw(Register dst, Register src) {
  emit_rex(rex_bits(dst, src));
  emit_byte(0x0F);
  emit_byte(0xBF);
  emit_byte(0xC0 | (dst.number() & 7) << 3 | (src.number() & 7));
}


void X86Assembler::cmovccl(Condition cc, Register dst, int imm32) {
  Unimplemented();
}


void X86Assembler::cmovccl(Condition cc, Register dst, oop obj) {
  Unimplemented();
}


void X86Assembler::cmovccl(Condition cc, Register dst, Register src) {
  Unimplemented();
}


void X86Assembler::cmovccl(Condition cc, Register dst, Address src) {
  Unimplemented();
}


void X86Assembler::adcl(Register dst, int imm32) {
  emit_arith(0x81, 0xD0, dst, imm32);
}


void X86Assembler::adcl(Register dst, Register src) {
  emit_arith(0x13, 0xC0, dst, src);
}


void X86Assembler::addl(Address dst, int imm32) {
  emit_rex(rex_bits(noreg, dst._base, dst._index));
  emit_byte(0x81);
  emit_operand(eax, dst);
  emit_long(imm32);
}


void X86Assembler::addl(Register dst, int imm32) {
  emit_arith(0x81, 0xC0, dst, imm32);
}


void X86Assembler::addl(Register dst, Register src) {
  emit_arith(0x03, 0xC0, dst, src);
}

void X86Assembler::addl(Register dst, Address  src) {
  emit_rex(rex_bits(dst, src._base, src._index));
  emit_byte(0x03);
  emit_operand(dst, src);
}


void X86Assembler::andl(Register dst, int imm32) {
  emit_arith(0x81, 0xE0, dst, imm32);
}


void X86Assembler::andl(Register dst, Register src) {
  emit_arith(0x23, 0xC0, dst, src);
}


void X86Assembler::cmpl(Address dst, int imm32) {
  emit_rex(rex_bits(noreg, dst._base, dst._index));
  emit_byte(0x81);
  emit_operand(edi, dst);
  emit_long(imm32);
}


void X86Assembler::cmpl(Address dst, oop obj) {
  assert(BytesPerNativeWord == 4, "an oop does not fit into a cmp r/m32, imm32 on 64-bit - materialize the oop in a register (movq) first");
  emit_byte(0x81);
  emit_operand(edi, dst);
  emit_data((intptr_t)obj, relocInfo::oop_type);
}


void X86Assembler::cmpl(Register dst, int imm32) {
  emit_arith(0x81, 0xF8, dst, imm32);
}


void X86Assembler::cmpl(Register dst, oop obj) {
  emit_arith(0x81, 0xF8, dst, obj);
}


void X86Assembler::cmpl(Register dst, Register src) {
  emit_arith(0x3B, 0xC0, dst, src);
}


void X86Assembler::cmpl(Register dst, Address  src) {
  emit_rex(rex_bits(dst, src._base, src._index));
  emit_byte(0x3B);
  emit_operand(dst, src);
}


void X86Assembler::decb(Register dst) {
  guarantee(dst.hasByteRegister(), "must have byte register");
  emit_byte(0xFE);
  emit_byte(0xC8 | dst.number());
}


void X86Assembler::decl(Register dst) {
#if DELTA_X86_64
  // the single-byte 0x48+rd form is a REX prefix in 64-bit mode, so use the
  // FF /1 (dec r/m32) encoding instead
  emit_rex(rex_bits(noreg, dst, noreg));
  emit_byte(0xFF);
  emit_byte(0xC8 | (dst.number() & 7));
#else
  emit_byte(0x48 | dst.number());
#endif
}

void X86Assembler::decl(Address dst) {
  emit_rex(rex_bits(noreg, dst._base, dst._index));
  emit_byte(0xFF);
  emit_operand(ecx, dst);
}

void X86Assembler::idivl(Register src) {
  emit_rex(rex_bits(noreg, src, noreg));
  emit_byte(0xF7);
  emit_byte(0xF8 | (src.number() & 7));
}

void X86Assembler::imull(Register src) {
  emit_rex(rex_bits(noreg, src, noreg));
  emit_byte(0xF7);
  emit_byte(0xE8 | (src.number() & 7));
}

void X86Assembler::imull(Register dst, Register src) {
  emit_rex(rex_bits(dst, src));
  emit_byte(0x0F);
  emit_byte(0xAF);
  emit_byte(0xC0 | (dst.number() & 7) << 3 | (src.number() & 7));
}


void X86Assembler::imull(Register dst, Register src, int value) {
  emit_rex(rex_bits(dst, src));
  if (is8bit(value)) {
    emit_byte(0x6B);
    emit_byte(0xC0 | (dst.number() & 7) << 3 | (src.number() & 7));
    emit_byte(value);
  } else {
    emit_byte(0x69);
    emit_byte(0xC0 | (dst.number() & 7) << 3 | (src.number() & 7));
    emit_long(value);
  }
}


void X86Assembler::incl(Register dst) {
#if DELTA_X86_64
  // the single-byte 0x40+rd form is a REX prefix in 64-bit mode, so use the
  // FF /0 (inc r/m32) encoding instead
  emit_rex(rex_bits(noreg, dst, noreg));
  emit_byte(0xFF);
  emit_byte(0xC0 | (dst.number() & 7));
#else
  emit_byte(0x40 | dst.number());
#endif
}


void X86Assembler::incl(Address dst) {
  emit_rex(rex_bits(noreg, dst._base, dst._index));
  emit_byte(0xFF);
  emit_operand(eax, dst);
}


void X86Assembler::leal(Register dst, Address src) {
  emit_rex(rex_bits(dst, src._base, src._index));
  emit_byte(0x8D);
  emit_operand(dst, src);
}


void X86Assembler::mull(Register src) {
  emit_rex(rex_bits(noreg, src, noreg));
  emit_byte(0xF7);
  emit_byte(0xE0 | (src.number() & 7));
}


void X86Assembler::negl(Register dst) {
  emit_rex(rex_bits(noreg, dst, noreg));
  emit_byte(0xF7);
  emit_byte(0xD8 | (dst.number() & 7));
}


void X86Assembler::notl(Register dst) {
  emit_rex(rex_bits(noreg, dst, noreg));
  emit_byte(0xF7);
  emit_byte(0xD0 | (dst.number() & 7));
}


void X86Assembler::orl(Register dst, int imm32) {
  emit_arith(0x81, 0xC8, dst, imm32);
}


void X86Assembler::orl(Register dst, Register src) {
  emit_arith(0x0B, 0xC0, dst, src);
}



void X86Assembler::orl(Register dst, Address src) {
  emit_rex(rex_bits(dst, src._base, src._index));
  emit_byte(0x0B);
  emit_operand(dst, src);
}


void X86Assembler::rcll(Register dst, int imm8) {
  assert(isShiftCount(imm8), "illegal shift count");
  emit_rex(rex_bits(noreg, dst, noreg));
  if (imm8 == 1) {
    emit_byte(0xD1);
    emit_byte(0xD0 | (dst.number() & 7));
  } else {
    emit_byte(0xC1);
    emit_byte(0xD0 | (dst.number() & 7));
    emit_byte(imm8);
  }
}


void X86Assembler::sarl(Register dst, int imm8) {
  assert(isShiftCount(imm8), "illegal shift count");
  emit_rex(rex_bits(noreg, dst, noreg));
  if (imm8 == 1) {
    emit_byte(0xD1);
    emit_byte(0xF8 | (dst.number() & 7));
  } else {
    emit_byte(0xC1);
    emit_byte(0xF8 | (dst.number() & 7));
    emit_byte(imm8);
  }
}


void X86Assembler::sarl(Register dst) {
  emit_rex(rex_bits(noreg, dst, noreg));
  emit_byte(0xD3);
  emit_byte(0xF8 | (dst.number() & 7));
}


void X86Assembler::sbbl(Register dst, int imm32) {
  Unimplemented();
}


void X86Assembler::sbbl(Register dst, Register src) {
  emit_arith(0x1B, 0xC0, dst, src);
}


void X86Assembler::shldl(Register dst, Register src) {
  emit_rex(rex_bits(src, dst));
  emit_byte(0x0F);
  emit_byte(0xA5);
  emit_byte(0xC0 | (src.number() & 7) << 3 | (dst.number() & 7));
}


void X86Assembler::shll(Register dst, int imm8) {
  assert(isShiftCount(imm8), "illegal shift count");
  emit_rex(rex_bits(noreg, dst, noreg));
  if (imm8 == 1 ) {
    emit_byte(0xD1);
    emit_byte(0xE0 | (dst.number() & 7));
  } else {
    emit_byte(0xC1);
    emit_byte(0xE0 | (dst.number() & 7));
    emit_byte(imm8);
  }
}


void X86Assembler::shll(Register dst) {
  emit_rex(rex_bits(noreg, dst, noreg));
  emit_byte(0xD3);
  emit_byte(0xE0 | (dst.number() & 7));
}


void X86Assembler::shrdl(Register dst, Register src) {
  emit_rex(rex_bits(src, dst));
  emit_byte(0x0F);
  emit_byte(0xAD);
  emit_byte(0xC0 | (src.number() & 7) << 3 | (dst.number() & 7));
}


void X86Assembler::shrl(Register dst, int imm8) {
  assert(isShiftCount(imm8), "illegal shift count");
  emit_rex(rex_bits(noreg, dst, noreg));
  if (imm8 == 1) {
    emit_byte(0xD1);
    emit_byte(0xE8 | (dst.number() & 7));
  } else {
    emit_byte(0xC1);
    emit_byte(0xE8 | (dst.number() & 7));
    emit_byte(imm8);
  }
}


void X86Assembler::shrl(Register dst) {
  emit_rex(rex_bits(noreg, dst, noreg));
  emit_byte(0xD3);
  emit_byte(0xE8 | (dst.number() & 7));
}


void X86Assembler::subl(Register dst, int imm32) {
  emit_arith(0x81, 0xE8, dst, imm32);
}


void X86Assembler::subl(Register dst, Register src) {
  emit_arith(0x2B, 0xC0, dst, src);
}

void X86Assembler::subl(Register dst, Address src) {
  emit_rex(rex_bits(dst, src._base, src._index));
  emit_byte(0x2B);
  emit_operand(dst, src);
}


void X86Assembler::testb(Register dst, int imm8) {
  guarantee(dst.hasByteRegister(), "must have byte register");
  emit_arith_b(0xF6, 0xC0, dst, imm8);
}


void X86Assembler::testl(Register dst, int imm32) {
  // not using emit_arith because test
  // doesn't support sign-extension of
  // 8bit operands
  if (dst.number() == 0) {
    emit_byte(0xA9);
  } else {
    emit_rex(rex_bits(noreg, dst, noreg));
    emit_byte(0xF7);
    emit_byte(0xC0 | (dst.number() & 7));
  }
  emit_long(imm32);
}


void X86Assembler::testl(Register dst, Register src) {
  emit_arith(0x85, 0xC0, dst, src);
}


void X86Assembler::xorl(Register dst, int imm32) {
  emit_arith(0x81, 0xF0, dst, imm32);
}


void X86Assembler::xorl(Register dst, Register src) {
  emit_arith(0x33, 0xC0, dst, src);
}

void X86Assembler::cdq() {
  emit_byte(0x99);
}

void X86Assembler::cqo() {
  assert(BytesPerNativeWord == 8, "cqo is only available on 64-bit");
  emit_rex_w();
  emit_byte(0x99);
}


// 64-bit arithmetic (x86-64)

void X86Assembler::addq(Address dst, int imm32) {
  assert(BytesPerNativeWord == 8, "addq is only available on 64-bit");
  emit_rex_w(noreg, dst._base, dst._index);
  emit_byte(0x81);
  emit_operand(eax, dst);
  emit_long(imm32);
}

void X86Assembler::addq(Register dst, int imm32) {
  assert(BytesPerNativeWord == 8, "addq is only available on 64-bit");
  emit_arith(0x81, 0xC0, dst, imm32, true);
}

void X86Assembler::addq(Register dst, Register src) {
  assert(BytesPerNativeWord == 8, "addq is only available on 64-bit");
  emit_arith(0x03, 0xC0, dst, src, true);
}

void X86Assembler::addq(Register dst, Address src) {
  assert(BytesPerNativeWord == 8, "addq is only available on 64-bit");
  emit_rex_w(dst, src._base, src._index);
  emit_byte(0x03);
  emit_operand(dst, src);
}


void X86Assembler::andq(Register dst, int imm32) {
  assert(BytesPerNativeWord == 8, "andq is only available on 64-bit");
  emit_arith(0x81, 0xE0, dst, imm32, true);
}

void X86Assembler::andq(Register dst, Register src) {
  assert(BytesPerNativeWord == 8, "andq is only available on 64-bit");
  emit_arith(0x23, 0xC0, dst, src, true);
}


void X86Assembler::cmpq(Address dst, int imm32) {
  assert(BytesPerNativeWord == 8, "cmpq is only available on 64-bit");
  emit_rex_w(noreg, dst._base, dst._index);
  emit_byte(0x81);
  emit_operand(edi, dst);
  emit_long(imm32);
}

void X86Assembler::cmpq(Register dst, int imm32) {
  assert(BytesPerNativeWord == 8, "cmpq is only available on 64-bit");
  emit_arith(0x81, 0xF8, dst, imm32, true);
}

void X86Assembler::cmpq(Register dst, Register src) {
  assert(BytesPerNativeWord == 8, "cmpq is only available on 64-bit");
  emit_arith(0x3B, 0xC0, dst, src, true);
}

void X86Assembler::cmpq(Register dst, Address src) {
  assert(BytesPerNativeWord == 8, "cmpq is only available on 64-bit");
  emit_rex_w(dst, src._base, src._index);
  emit_byte(0x3B);
  emit_operand(dst, src);
}


void X86Assembler::decq(Register dst) {
  assert(BytesPerNativeWord == 8, "decq is only available on 64-bit");
  emit_rex_w(noreg, dst, noreg);
  emit_byte(0xFF);
  emit_byte(0xC8 | (dst.number() & 7));
}

void X86Assembler::decq(Address dst) {
  assert(BytesPerNativeWord == 8, "decq is only available on 64-bit");
  emit_rex_w(noreg, dst._base, dst._index);
  emit_byte(0xFF);
  emit_operand(ecx, dst);
}


void X86Assembler::incq(Register dst) {
  assert(BytesPerNativeWord == 8, "incq is only available on 64-bit");
  emit_rex_w(noreg, dst, noreg);
  emit_byte(0xFF);
  emit_byte(0xC0 | (dst.number() & 7));
}

void X86Assembler::incq(Address dst) {
  assert(BytesPerNativeWord == 8, "incq is only available on 64-bit");
  emit_rex_w(noreg, dst._base, dst._index);
  emit_byte(0xFF);
  emit_operand(eax, dst);
}


void X86Assembler::leaq(Register dst, Address src) {
  assert(BytesPerNativeWord == 8, "leaq is only available on 64-bit");
  emit_rex_w(dst, src._base, src._index);
  emit_byte(0x8D);
  emit_operand(dst, src);
}


void X86Assembler::idivq(Register src) {
  assert(BytesPerNativeWord == 8, "idivq is only available on 64-bit");
  emit_rex_w(noreg, src, noreg);
  emit_byte(0xF7);
  emit_byte(0xF8 | (src.number() & 7));
}

void X86Assembler::imulq(Register dst, Register src) {
  assert(BytesPerNativeWord == 8, "imulq is only available on 64-bit");
  emit_rex_w(dst, src);
  emit_byte(0x0F);
  emit_byte(0xAF);
  emit_byte(0xC0 | (dst.number() & 7) << 3 | (src.number() & 7));
}

void X86Assembler::imulq(Register dst, Register src, int value) {
  assert(BytesPerNativeWord == 8, "imulq is only available on 64-bit");
  emit_rex_w(dst, src);
  if (is8bit(value)) {
    emit_byte(0x6B);
    emit_byte(0xC0 | (dst.number() & 7) << 3 | (src.number() & 7));
    emit_byte(value);
  } else {
    emit_byte(0x69);
    emit_byte(0xC0 | (dst.number() & 7) << 3 | (src.number() & 7));
    emit_long(value);
  }
}

void X86Assembler::mulq(Register src) {
  assert(BytesPerNativeWord == 8, "mulq is only available on 64-bit");
  emit_rex_w(noreg, src, noreg);
  emit_byte(0xF7);
  emit_byte(0xE0 | (src.number() & 7));
}


void X86Assembler::negq(Register dst) {
  assert(BytesPerNativeWord == 8, "negq is only available on 64-bit");
  emit_rex_w(noreg, dst, noreg);
  emit_byte(0xF7);
  emit_byte(0xD8 | (dst.number() & 7));
}

void X86Assembler::notq(Register dst) {
  assert(BytesPerNativeWord == 8, "notq is only available on 64-bit");
  emit_rex_w(noreg, dst, noreg);
  emit_byte(0xF7);
  emit_byte(0xD0 | (dst.number() & 7));
}


void X86Assembler::orq(Register dst, int imm32) {
  assert(BytesPerNativeWord == 8, "orq is only available on 64-bit");
  emit_arith(0x81, 0xC8, dst, imm32, true);
}

void X86Assembler::orq(Register dst, Register src) {
  assert(BytesPerNativeWord == 8, "orq is only available on 64-bit");
  emit_arith(0x0B, 0xC0, dst, src, true);
}

void X86Assembler::orq(Register dst, Address src) {
  assert(BytesPerNativeWord == 8, "orq is only available on 64-bit");
  emit_rex_w(dst, src._base, src._index);
  emit_byte(0x0B);
  emit_operand(dst, src);
}


void X86Assembler::sarq(Register dst, int imm8) {
  assert(BytesPerNativeWord == 8, "sarq is only available on 64-bit");
  assert(isShiftCount(imm8), "illegal shift count");
  emit_rex_w(noreg, dst, noreg);
  if (imm8 == 1) {
    emit_byte(0xD1);
    emit_byte(0xF8 | (dst.number() & 7));
  } else {
    emit_byte(0xC1);
    emit_byte(0xF8 | (dst.number() & 7));
    emit_byte(imm8);
  }
}

void X86Assembler::shlq(Register dst, int imm8) {
  assert(BytesPerNativeWord == 8, "shlq is only available on 64-bit");
  assert(isShiftCount(imm8), "illegal shift count");
  emit_rex_w(noreg, dst, noreg);
  if (imm8 == 1) {
    emit_byte(0xD1);
    emit_byte(0xE0 | (dst.number() & 7));
  } else {
    emit_byte(0xC1);
    emit_byte(0xE0 | (dst.number() & 7));
    emit_byte(imm8);
  }
}

void X86Assembler::shrq(Register dst, int imm8) {
  assert(BytesPerNativeWord == 8, "shrq is only available on 64-bit");
  assert(isShiftCount(imm8), "illegal shift count");
  emit_rex_w(noreg, dst, noreg);
  if (imm8 == 1) {
    emit_byte(0xD1);
    emit_byte(0xE8 | (dst.number() & 7));
  } else {
    emit_byte(0xC1);
    emit_byte(0xE8 | (dst.number() & 7));
    emit_byte(imm8);
  }
}

void X86Assembler::shlq(Register dst) {
  assert(BytesPerNativeWord == 8, "shlq is only available on 64-bit");
  emit_rex_w(noreg, dst, noreg);
  emit_byte(0xD3);
  emit_byte(0xE0 | (dst.number() & 7));
}

void X86Assembler::sarq(Register dst) {
  assert(BytesPerNativeWord == 8, "sarq is only available on 64-bit");
  emit_rex_w(noreg, dst, noreg);
  emit_byte(0xD3);
  emit_byte(0xF8 | (dst.number() & 7));
}

void X86Assembler::shrq(Register dst) {
  assert(BytesPerNativeWord == 8, "shrq is only available on 64-bit");
  emit_rex_w(noreg, dst, noreg);
  emit_byte(0xD3);
  emit_byte(0xE8 | (dst.number() & 7));
}


void X86Assembler::subq(Register dst, int imm32) {
  assert(BytesPerNativeWord == 8, "subq is only available on 64-bit");
  emit_arith(0x81, 0xE8, dst, imm32, true);
}

void X86Assembler::subq(Register dst, Register src) {
  assert(BytesPerNativeWord == 8, "subq is only available on 64-bit");
  emit_arith(0x2B, 0xC0, dst, src, true);
}

void X86Assembler::subq(Register dst, Address src) {
  assert(BytesPerNativeWord == 8, "subq is only available on 64-bit");
  emit_rex_w(dst, src._base, src._index);
  emit_byte(0x2B);
  emit_operand(dst, src);
}


void X86Assembler::testq(Register dst, int imm32) {
  assert(BytesPerNativeWord == 8, "testq is only available on 64-bit");
  if (dst.number() == 0) {
    emit_rex_w();
    emit_byte(0xA9);
  } else {
    emit_rex_w(noreg, dst, noreg);
    emit_byte(0xF7);
    emit_byte(0xC0 | (dst.number() & 7));
  }
  emit_long(imm32);
}

void X86Assembler::testq(Register dst, Register src) {
  assert(BytesPerNativeWord == 8, "testq is only available on 64-bit");
  emit_arith(0x85, 0xC0, dst, src, true);
}


void X86Assembler::xorq(Register dst, int imm32) {
  assert(BytesPerNativeWord == 8, "xorq is only available on 64-bit");
  emit_arith(0x81, 0xF0, dst, imm32, true);
}

void X86Assembler::xorq(Register dst, Register src) {
  assert(BytesPerNativeWord == 8, "xorq is only available on 64-bit");
  emit_arith(0x33, 0xC0, dst, src, true);
}

void X86Assembler::hlt() {
  emit_byte(0xF4);
}


void X86Assembler::int3() {
  if (EnableInt3) emit_byte(0xCC);
}


void X86Assembler::nop() {
  emit_byte(0x90);
}


void X86Assembler::ret(int imm16) {
  if (imm16 == 0) {
    emit_byte(0xC3);
  } else {
    emit_byte(0xC2);
    emit_byte(imm16 & 0xFF);
    emit_byte((imm16 >> 8) & 0xFF);
  }
}


// Labels refer to positions in the (to be) generated code.
// There are bound, unbound and undefined labels.
//
// Bound labels refer to known positions in the already
// generated code. pos() is the position the label refers to.
//
// Unbound labels refer to unknown positions in the code
// to be generated; pos() is the position of the 32bit
// Displacement of the last instruction using the label.
//
// Undefined labels are labels that haven't been used yet.
// They refer to no position at all.


void X86Assembler::print(Label& L) {
  if (L.is_unused()) {
    mystd->print_cr("undefined label");
  } else if (L.is_bound()) {
    mystd->print_cr("bound label to %d", L.pos());
  } else if (L.is_unbound()) {
    Label l = L;
    mystd->print_cr("unbound label");
    while (l.is_unbound()) {
      Displacement disp = disp_at(l);
      mystd->print("@ %d ", l.pos());
      disp.print();
      mystd->cr();
      disp.next(l);
    }
  } else {
    mystd->print_cr("label in inconsistent state (pos = %d)", L._pos);
  }
}


void X86Assembler::bind_to(Label& L, int pos) {
  bool tellRobert = false;

  assert(0 <= pos && pos <= offset(), "must have a valid binding position");
  while (L.is_unbound()) {
    Displacement disp = disp_at(L);
    int fixup_pos = L.pos();
    int imm32 = 0;
    switch (disp.type()) {
      case Displacement::call:
        { assert(byte_at(fixup_pos - 1) == 0xE8, "call expected");
          imm32 = pos - (fixup_pos + sizeof(int));
        }
	break;
      case Displacement::absolute_jump:
        { assert(byte_at(fixup_pos - 1) == 0xE9, "jmp expected");
          imm32 = pos - (fixup_pos + sizeof(int));
	  if (imm32 == 0 && EliminateJumpsToJumps) tellRobert = true;
        }
	break;
      case Displacement::conditional_jump:
        { assert(byte_at(fixup_pos - 2) == 0x0F, "jcc expected");
          assert(byte_at(fixup_pos - 1) == (0x80 | disp.info()), "jcc expected");
          imm32 = pos - (fixup_pos + sizeof(int));
	}
	break;
      case Displacement::ic_info:
        { assert(byte_at(fixup_pos - 1) == 0xA9, "test eax expected");
	  int offs = pos - (fixup_pos - IC_Info::info_offset);
	  assert(((offs << IC_Info::number_of_flags) >> IC_Info::number_of_flags) == offs, "NLR offset out of bounds");
	  imm32 = (offs << IC_Info::number_of_flags) | disp.info();
	}
        break;
      default:
        ShouldNotReachHere();
    }
    long_at_put(fixup_pos, imm32);
    disp.next(L);
  }
  L.bind_to(pos);

  if (tellRobert) {
    //warning("jmp to next has not been eliminated - tell Robert, please");
    code()->decode();
  }
}


void X86Assembler::link_to(Label& L, Label& appendix) {
  if (appendix.is_unbound()) {
    if (L.is_unbound()) {
      // append appendix to L's list
      Label p, q = L;
      do { p = q; disp_at(q).next(q); } while (q.is_unbound());
      Displacement disp = disp_at(p);
      disp.link_to(appendix);
      disp_at_put(p, disp);
      p.unuse(); // to avoid assertion failure in ~Label
    } else {
      // L is empty, simply use appendix
      L = appendix;
    }
  }
  appendix.unuse(); // appendix should not be used anymore
}


void X86Assembler::bind(Label& L) {
  assert(!L.is_bound(), "label can only be bound once");
  if (EliminateJumpsToJumps) {
    // resolve unbound label
    if (_unbound_label.is_unbound()) {
      // unbound label exists => link it with L if same binding position, otherwise fix it
      if (_binding_pos == offset()) {
        // link it to L's list
        link_to(L, _unbound_label);
      } else {
        // otherwise bind unbound label
	assert(_binding_pos < offset(), "assembler error");
        bind_to(_unbound_label, _binding_pos);
      }
    }
    assert(!_unbound_label.is_unbound(), "assembler error");
    // try to eliminate jumps to next instruction
    while (L.is_unbound() && (L.pos() + int(sizeof(int)) == offset()) && (disp_at(L).type() == Displacement::absolute_jump)) {
      // previous instruction is jump jumping immediately after it => eliminate it
      const int long_size = 5;
      assert(byte_at(offset() - long_size) == 0xE9, "jmp expected");
      if (PrintJumpElimination) mystd->print_cr("@ %d jump to next eliminated", L.pos());
      // remove first entry from label list
      disp_at(L).next(L);
      // eliminate instruction (set code pointers back)
      _code_pos -= long_size;
      code()->set_code_end(_code_pos);
    }
    // delay fixup of L => store it as unbound label
    _unbound_label = L;
    _binding_pos = offset();
    L.unuse();
  }
  bind_to(L, offset());
}


void X86Assembler::merge(Label& L, Label& with) {
  Unimplemented();
}


void X86Assembler::call(Label& L) {
  if (L.is_bound()) {
    const int long_size = 5;
    int offs = L.pos() - offset();
    assert(offs <= 0, "assembler error");
    // 1110 1000 #32-bit disp
    emit_byte(0xE8);
    emit_long(offs - long_size);
  } else {
    // 1110 1000 #32-bit disp
    emit_byte(0xE8);
    emit_disp(L, Displacement::call, 0);
  }
}


void X86Assembler::call(char* entry, relocInfo::relocType rtype) {
  // rel32 is relative to the next instruction, i.e. the end of the 4-byte
  // displacement field (sizeof(int)); sizeof(long) would be 8 on LP64.
  emit_byte(0xE8);
  emit_data((intptr_t)entry - ((intptr_t)_code_pos + sizeof(int)), rtype);
}


void X86Assembler::call(Register dst) {
  emit_rex(rex_bits(noreg, dst, noreg));
  emit_byte(0xFF);
  emit_byte(0xD0 | (dst.number() & 7));
}


void X86Assembler::call(Address adr) {
  emit_rex(rex_bits(noreg, adr._base, adr._index));
  emit_byte(0xFF);
  emit_operand(edx, adr);
}


void X86Assembler::jmp(char* entry, relocInfo::relocType rtype) {
  emit_byte(0xE9);
  emit_data((intptr_t)entry - ((intptr_t)_code_pos + sizeof(int)), rtype);
}


void X86Assembler::jmp(Register reg) {
  emit_rex(rex_bits(noreg, reg, noreg));
  emit_byte(0xFF);
  emit_byte(0xE0 | (reg.number() & 7));
}


void X86Assembler::jmp(Address adr) {
  emit_rex(rex_bits(noreg, adr._base, adr._index));
  emit_byte(0xFF);
  emit_operand(esp, adr);
}


void X86Assembler::jmp(Label& L) {
  if (L.is_bound()) {
    const int short_size = 2;
    const int long_size  = 5;
    int offs = L.pos() - offset();
    assert(offs <= 0, "assembler error");
    if (is8bit(offs - short_size)) {
      // 1110 1011 #8-bit disp
      emit_byte(0xEB);
      emit_byte((offs - short_size) & 0xFF);
    } else {
      // 1110 1001 #32-bit disp
      emit_byte(0xE9);
      emit_long(offs - long_size);
    }
  } else {
    if (EliminateJumpsToJumps && _unbound_label.is_unbound() && _binding_pos == offset()) {
      // current position is target of jumps
      if (PrintJumpElimination) {
        mystd->print_cr("eliminated jumps/calls to %d", _binding_pos);
        mystd->print("from ");
        print(_unbound_label);
      }
      link_to(L, _unbound_label);
    }
    // 1110 1001 #32-bit disp
    emit_byte(0xE9);
    emit_disp(L, Displacement::absolute_jump, 0);
  }
}


void X86Assembler::jcc(Condition cc, Label& L) {
  assert((0 <= cc) && (cc < 16), "illegal cc");
  if (L.is_bound()) {
    const int short_size = 2;
    const int long_size  = 6;
    int offs = L.pos() - offset();
    assert(offs <= 0, "assembler error");
    if (is8bit(offs - short_size)) {
      // 0111 tttn #8-bit disp
      emit_byte(0x70 | cc);
      emit_byte((offs - short_size) & 0xFF);
    } else {
      // 0000 1111 1000 tttn #32-bit disp
      emit_byte(0x0F);
      emit_byte(0x80 | cc);
      emit_long(offs - long_size);
    }
  } else {
    // 0000 1111 1000 tttn #32-bit disp
    // Note: could eliminate cond. jumps to this jump if condition
    //       is the same however, seems to be rather unlikely case.
    emit_byte(0x0F);
    emit_byte(0x80 | cc);
    emit_disp(L, Displacement::conditional_jump, cc);
  }
}


void X86Assembler::jcc(Condition cc, char* dst, relocInfo::relocType rtype) {
  assert((0 <= cc) && (cc < 16), "illegal cc");
  // 0000 1111 1000 tttn #32-bit disp
  emit_byte(0x0F);
  emit_byte(0x80 | cc);
  emit_data((intptr_t)dst - ((intptr_t)_code_pos + sizeof(int)), rtype);
}


void X86Assembler::ic_info(Label& L, int flags) {
  assert((unsigned int)flags >> IC_Info::number_of_flags == 0, "too many flags set");
  if (L.is_bound()) {
    int offs = L.pos() - offset();
    assert(offs <= 0, "assembler error");
    assert(((offs << IC_Info::number_of_flags) >> IC_Info::number_of_flags) == offs, "NLR offset out of bounds");
    emit_byte(0xA9);
    emit_long((offs << IC_Info::number_of_flags) | flags);
  } else {
    emit_byte(0xA9);
    emit_disp(L, Displacement::ic_info, flags);
  }
}


// FPU instructions

void X86Assembler::fld1() {
  emit_byte(0xD9);
  emit_byte(0xE8);
}


void X86Assembler::fldz() {
  emit_byte(0xD9);
  emit_byte(0xEE);
}


void X86Assembler::fld_s(Address adr) {
  emit_rex(rex_bits(eax, adr._base, adr._index));
  emit_byte(0xD9);
  emit_operand(eax, adr);
}


void X86Assembler::fld_d(Address adr) {
  emit_rex(rex_bits(eax, adr._base, adr._index));
  emit_byte(0xDD);
  emit_operand(eax, adr);
}


void X86Assembler::fstp_s(Address adr) {
  emit_rex(rex_bits(ebx, adr._base, adr._index));
  emit_byte(0xD9);
  emit_operand(ebx, adr);
}


void X86Assembler::fstp_d(Address adr) {
  emit_rex(rex_bits(ebx, adr._base, adr._index));
  emit_byte(0xDD);
  emit_operand(ebx, adr);
}


void X86Assembler::fild_s(Address adr) {
  emit_rex(rex_bits(eax, adr._base, adr._index));
  emit_byte(0xDB);
  emit_operand(eax, adr);
}


void X86Assembler::fild_d(Address adr) {
  emit_rex(rex_bits(ebp, adr._base, adr._index));
  emit_byte(0xDF);
  emit_operand(ebp, adr);
}


void X86Assembler::fistp_s(Address adr) {
  emit_rex(rex_bits(ebx, adr._base, adr._index));
  emit_byte(0xDB);
  emit_operand(ebx, adr);
}


void X86Assembler::fistp_d(Address adr) {
  emit_rex(rex_bits(edi, adr._base, adr._index));
  emit_byte(0xDF);
  emit_operand(edi, adr);
}


void X86Assembler::fabs() {
  emit_byte(0xD9);
  emit_byte(0xE1);
}


void X86Assembler::fchs() {
  emit_byte(0xD9);
  emit_byte(0xE0);
}

void X86Assembler::fadd_d(Address adr) {
  emit_rex(rex_bits(eax, adr._base, adr._index));
  emit_byte(0xDC);
  emit_operand(eax, adr);
}

void X86Assembler::fsub_d(Address adr) {
  emit_rex(rex_bits(esp, adr._base, adr._index));
  emit_byte(0xDC);
  emit_operand(esp, adr);
}

void X86Assembler::fmul_d(Address adr) {
  emit_rex(rex_bits(ecx, adr._base, adr._index));
  emit_byte(0xDC);
  emit_operand(ecx, adr);
}

void X86Assembler::fdiv_d(Address adr) {
  emit_rex(rex_bits(esi, adr._base, adr._index));
  emit_byte(0xDC);
  emit_operand(esi, adr);
}

void X86Assembler::fadd(int i) {
  emit_farith(0xDC, 0xC0, i);
}


void X86Assembler::fsub(int i) {
  emit_farith(0xDC, 0xE8, i);
}


void X86Assembler::fmul(int i) {
  emit_farith(0xDC, 0xC8, i);
}


void X86Assembler::fdiv(int i) {
  emit_farith(0xDC, 0xF8, i);
}


void X86Assembler::faddp(int i) {
  emit_farith(0xDE, 0xC0, i);
}


void X86Assembler::fsubp(int i) {
  emit_farith(0xDE, 0xE8, i);
}


void X86Assembler::fsubrp(int i) {
  emit_farith(0xDE, 0xE0, i);
}


void X86Assembler::fmulp(int i) {
  emit_farith(0xDE, 0xC8, i);
}


void X86Assembler::fdivp(int i) {
  emit_farith(0xDE, 0xF8, i);
}



void X86Assembler::fprem() {
  emit_byte(0xD9);
  emit_byte(0xF8);
}


void X86Assembler::fprem1() {
  emit_byte(0xD9);
  emit_byte(0xF5);
}


void X86Assembler::fxch(int i) {
  emit_farith(0xD9, 0xC8, i);
}


void X86Assembler::fincstp() {
  emit_byte(0xD9);
  emit_byte(0xF7);
}


void X86Assembler::ffree(int i) {
  emit_farith(0xDD, 0xC0, i);
}


void X86Assembler::ftst() {
  emit_byte(0xD9);
  emit_byte(0xE4);
}


void X86Assembler::fcompp() {
  emit_byte(0xDE);
  emit_byte(0xD9);
}


void X86Assembler::fnstsw_ax() {
  emit_byte(0xdF);
  emit_byte(0xE0);
}


void X86Assembler::fwait() {
  emit_byte(0x9B);
}


// Implementation of X86MacroAssembler

void X86MacroAssembler::align(int modulus) {
  while (offset() % modulus != 0) nop();
}


void X86MacroAssembler::test(Register dst, int imm8) {
  if (!CodeForP6 && dst.hasByteRegister()) {
    testb(dst, imm8);
  } else {
    testl(dst, imm8);
  }
}


void X86MacroAssembler::enter() {
#if DELTA_X86_64
  pushq(ebp);
  movq(ebp, esp);
#else
  pushl(ebp);
  movl(ebp, esp);
#endif
}


void X86MacroAssembler::leave() {
#if DELTA_X86_64
  movq(esp, ebp);
  popq(ebp);
#else
  movl(esp, ebp);
  popl(ebp);
#endif
}


// Support for inlined data

void X86MacroAssembler::inline_oop(oop o) {
  emit_byte(0xA9);
  emit_data((intptr_t)o, relocInfo::oop_type);
}


// Calls to C land
//
// When entering C land, the ebp & esp of the last Delta frame have to be recorded.
// When leaving C land, last_Delta_fp has to be reset to 0. This is required to
// allow proper stack traversal.

void X86MacroAssembler::set_last_Delta_frame_before_call() {
  // Note: the absolute addresses of last_Delta_fp/sp are emitted as disp32
  // external_word_type references; on a 64-bit build these must be fixed up
  // to RIP-relative addresses by the relocation machinery (64-bit port item).
#if DELTA_X86_64
  movq(Address((intptr_t)&last_Delta_fp, relocInfo::external_word_type), ebp);
  movq(Address((intptr_t)&last_Delta_sp, relocInfo::external_word_type), esp);
#else
  movl(Address((intptr_t)&last_Delta_fp, relocInfo::external_word_type), ebp);
  movl(Address((intptr_t)&last_Delta_sp, relocInfo::external_word_type), esp);
#endif
}


void X86MacroAssembler::set_last_Delta_frame_after_call() {
#if DELTA_X86_64
  addq(esp, oopSize);	// sets esp to value before call (i.e., before pushing the return address)
  set_last_Delta_frame_before_call();
  subq(esp, oopSize);	// resets esp to original value
#else
  addl(esp, oopSize);	// sets esp to value before call (i.e., before pushing the return address)
  set_last_Delta_frame_before_call();
  subl(esp, oopSize);	// resets esp to original value
#endif
}


void X86MacroAssembler::reset_last_Delta_frame() {
  // Note: see the note in set_last_Delta_frame_before_call() about the
  // absolute address on 64-bit builds.
#if DELTA_X86_64
  movq(Address((intptr_t)&last_Delta_fp, relocInfo::external_word_type), 0);
#else
  movl(Address((intptr_t)&last_Delta_fp, relocInfo::external_word_type), 0);
#endif
}


void X86MacroAssembler::call_C(Label& L) {
  set_last_Delta_frame_before_call();
  call(L);
  reset_last_Delta_frame();
}


void X86MacroAssembler::call_C(Label& L, Label& nlrTestPoint) {
  set_last_Delta_frame_before_call();
  call(L);
  X86Assembler::ic_info(nlrTestPoint, 0);
  reset_last_Delta_frame();
}


void X86MacroAssembler::call_C(char* entry, relocInfo::relocType rtype) {
  set_last_Delta_frame_before_call();
  call(entry, rtype);
  reset_last_Delta_frame();
}


void X86MacroAssembler::call_C(char* entry, relocInfo::relocType rtype, Label& nlrTestPoint) {
  set_last_Delta_frame_before_call();
  call(entry, rtype);
  X86Assembler::ic_info(nlrTestPoint, 0);
  reset_last_Delta_frame();
}


void X86MacroAssembler::call_C(Register entry) {
  set_last_Delta_frame_before_call();
  call(entry);
  reset_last_Delta_frame();
}


void X86MacroAssembler::call_C(Register entry, Label& nlrTestPoint) {
  set_last_Delta_frame_before_call();
  call(entry);
  X86Assembler::ic_info(nlrTestPoint, 0);
  reset_last_Delta_frame();
}


// The following 3 macros implement C run-time calls with arguments. When setting up the
// last Delta frame, the value pushed after last_Delta_sp MUST be a valid return address,
// therefore an additional call to a little stub is required which does the parameter
// passing.
//
// [return addr] \
// [argument 1 ]  |   extra stub in C land
//  ...           |
// [argument n ] /
// [return addr] <=== must be valid return address  \
// [...        ] <--- last_Delta_sp                  |
//  ...                                              | last Delta frame in Delta land
// [...        ]                                     |
// [previous fp] <--- last_Delta_fp                 /
//
// Note: The routines could be implemented slightly more efficient and shorter by
// explicitly pushing/popping a valid return address instead of calling the extra
// stub. However, currently the assembler doesn't support label pushes.


void X86MacroAssembler::call_C(char* entry, Register arg1) {
#if DELTA_X86_64
  // x86-64 SysV: first argument in rdi
  set_last_Delta_frame_before_call();
  movq(edi, arg1);	// edi == rdi on a 64-bit build
  call(entry, relocInfo::runtime_call_type);
  reset_last_Delta_frame();
#else
  Label L1, L2;
  jmp(L1);

  bind(L2);
  pushl(arg1);
  call(entry, relocInfo::runtime_call_type);
  addl(esp, 1*oopSize);
  ret(0);

  bind(L1);
  call_C(L2);
#endif
}


void X86MacroAssembler::call_C(char* entry, Register arg1, Register arg2) {
#if DELTA_X86_64
  // x86-64 SysV: arguments in rdi, rsi (arg registers must be distinct from
  // rdi/rsi so that the moves do not clobber each other)
  assert(arg1 != esi && arg2 != edi, "argument register overlap");
  set_last_Delta_frame_before_call();
  movq(edi, arg1);	// edi == rdi, esi == rsi on a 64-bit build
  movq(esi, arg2);
  call(entry, relocInfo::runtime_call_type);
  reset_last_Delta_frame();
#else
  Label L1, L2;
  jmp(L1);

  bind(L2);
  pushl(arg2);
  pushl(arg1);
  call(entry, relocInfo::runtime_call_type);
  addl(esp, 2*oopSize);
  ret(0);

  bind(L1);
  call_C(L2);
#endif
}


void X86MacroAssembler::call_C(char* entry, Register arg1, Register arg2, Register arg3) {
#if DELTA_X86_64
  // x86-64 SysV: arguments in rdi, rsi, rdx
  assert(arg1 != esi && arg1 != edx && arg2 != edi && arg2 != edx && arg3 != edi && arg3 != esi, "argument register overlap");
  set_last_Delta_frame_before_call();
  movq(edi, arg1);	// edi == rdi, esi == rsi, edx == rdx on a 64-bit build
  movq(esi, arg2);
  movq(edx, arg3);
  call(entry, relocInfo::runtime_call_type);
  reset_last_Delta_frame();
#else
  Label L1, L2;
  jmp(L1);

  bind(L2);
  pushl(arg3);
  pushl(arg2);
  pushl(arg1);
  call(entry, relocInfo::runtime_call_type);
  addl(esp, 3*oopSize);
  ret(0);

  bind(L1);
  call_C(L2);
#endif
}


void X86MacroAssembler::call_C(char* entry, Register arg1, Register arg2, Register arg3, Register arg4) {
#if DELTA_X86_64
  // x86-64 SysV: arguments in rdi, rsi, rdx, rcx
  assert(arg1 != esi && arg1 != edx && arg1 != ecx &&
         arg2 != edi && arg2 != edx && arg2 != ecx &&
         arg3 != edi && arg3 != esi && arg3 != ecx &&
         arg4 != edi && arg4 != esi && arg4 != edx, "argument register overlap");
  set_last_Delta_frame_before_call();
  movq(edi, arg1);	// edi == rdi, esi == rsi, edx == rdx, ecx == rcx on a 64-bit build
  movq(esi, arg2);
  movq(edx, arg3);
  movq(ecx, arg4);
  call(entry, relocInfo::runtime_call_type);
  reset_last_Delta_frame();
#else
  Label L1, L2;
  jmp(L1);

  bind(L2);
  pushl(arg4);
  pushl(arg3);
  pushl(arg2);
  pushl(arg1);
  call(entry, relocInfo::runtime_call_type);
  addl(esp, 4*oopSize);
  ret(0);

  bind(L1);
  call_C(L2);
#endif
}


void X86MacroAssembler::store_check(Register obj, Register tmp) {
  // Does a store check for the oop in register obj. The content of
  // register obj is destroyed afterwards.
  // Note: Could be optimized by hardwiring the byte map base address
  // in the code - however relocation would be necessary whenever the
  // base changes. Advantage: only one instead of two instructions.
  assert(obj != tmp, "registers must be different");
  Label no_store;
#if DELTA_X86_64
  // On 64-bit the boundary and the byte map base are full 64-bit pointers;
  // load them with movabs (they are code-generation-time constants).
  movq(tmp, (intptr_t)Universe::new_gen.boundary());   // assumes boundary between new_gen and old_gen is fixed
  cmpq(obj, tmp);                                      // avoid marking dirty if target is a new object
  jcc(X86Assembler::less, no_store);
  movq(tmp, (intptr_t)&byte_map_base);
  movq(tmp, Address(tmp));
  shrq(obj, card_shift);
  movb(Address(tmp, obj, Address::times_1), 0);
#else
  cmpl(obj, (intptr_t)Universe::new_gen.boundary());   // assumes boundary between new_gen and old_gen is fixed
  jcc(X86Assembler::less, no_store);                      // avoid marking dirty if target is a new object
  movl(tmp, Address((intptr_t)&byte_map_base, relocInfo::external_word_type));
  shrl(obj, card_shift);
  movb(Address(tmp, obj, Address::times_1), 0);
#endif
  bind(no_store);
}


void X86MacroAssembler::fpu_mask_and_cond_for(Condition cc, int& mask, Condition& cond) {
  switch (cc) {
    case equal		: mask = 0x4000; cond = notZero;	break;
    case notEqual	: mask = 0x4000; cond = zero;		break;
    case less		: mask = 0x0100; cond = notZero;	break;
    case lessEqual	: mask = 0x4500; cond = notZero;	break;
    case greater	: mask = 0x4500; cond = zero;		break;
    case greaterEqual	: mask = 0x0100; cond = zero;		break;
    default		: Unimplemented();
  };
}


void X86MacroAssembler::fpop() {
  ffree();
  fincstp();
}


// debugging

void X86MacroAssembler::print_reg(char* name, oop obj) {
  mystd->print("%s = ", name);
  if (obj == NULL) {
    mystd->print_cr("NULL");
  } else if (obj->is_smi()) {
    mystd->print_cr("smi (%d)", smiOop(obj)->value());
  } else if (obj->is_mem() && Universe::is_heap((oop*)obj)) {
    // use explicit checks to avoid crashes even in a broken system
    if (obj == Universe::nilObj()) {
      mystd->print_cr("nil (0x%08x)", obj);
    } else if (obj == Universe::trueObj()) {
      mystd->print_cr("true (0x%08x)", obj);
    } else if (obj == Universe::falseObj()) {
      mystd->print_cr("false (0x%08x)", obj);
    } else {
      mystd->print_cr("memOop (0x%08x)", obj);
    }
  } else {
    mystd->print_cr("0x%08x", obj);
  }
}


void X86MacroAssembler::inspector(oop edi, oop esi, oop ebp, oop esp, oop ebx, oop edx, oop ecx, oop eax, char* eip) {
  char* title = (char*)(nativeTest_at(eip)->data());
  if (title != NULL) mystd->print_cr("%s", title);
  print_reg("eax", eax);
  print_reg("ebx", ebx);
  print_reg("ecx", ecx);
  print_reg("edx", edx);
  print_reg("edi", edi);
  print_reg("esi", esi);
  mystd->print_cr("ebp = 0x%08x", ebp);
  mystd->print_cr("esp = 0x%08x", esp);
  mystd->cr();
}


void X86MacroAssembler::inspect(char* title) {
  char* entry = StubRoutines::call_inspector_entry();
  if (entry != NULL) {
    call(entry, relocInfo::runtime_call_type);			// call stub invoking the inspector
    testl(eax, intptr_t(title));				// additional info for inspector
  } else {
    const char* s = (title == NULL) ? "" : title;
    mystd->print_cr("cannot call inspector for \"%s\" - no entry point yet", s);
  }
}

#endif // !defined(DELTA_ASSEMBLER_BACKEND_AARCH64)
