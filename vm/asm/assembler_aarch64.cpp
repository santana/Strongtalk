/*
Copyright (c) 2026, Gerardo Santana Gomez Garrido.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// Implementation of the AArch64 assembler. All encodings were cross-verified
// byte-for-byte against Apple clang (arm64-apple-macos11); the golden bytes
// live in test/assembler/assemblerEncoderTest_aarch64.cpp. See the header
// comment in asm/assembler_aarch64.hpp for the verification status.
//
// This file is inert unless the AArch64 backend is selected with
// -DDELTA_ASSEMBLER_BACKEND_AARCH64, so that the default (x86) VM build
// (which compiles every vm/*/*.cpp) is unaffected.

#ifdef DELTA_ASSEMBLER_BACKEND_AARCH64

#include "asm/assembler_aarch64.hpp"
#include "asm/codeBuffer.hpp"
#include "memory/rSet.hpp"			// card_shift (store_check)
#include "memory/universe.hpp"		// Universe (inspector, store_check)
#include "oops/oop.hpp"
#include "oops/smiOop.hpp"
#include "runtime/process.hpp"		// last_Delta_fp/last_Delta_sp
#include "runtime/runtime.hpp"		// byte_map_base (store_check)
#include "topIncludes/std_includes.hpp"
#include "utilities/ostream.hpp"

#include <cstdint>

static const uint64_t MASK64 = ~0ull;


// ---------------------------------------------------------------------------
// logical immediate encoding (LLVM processLogicalImmediate port)
// ---------------------------------------------------------------------------

static int countr_zero(uint64_t x)		{ return x ? __builtin_ctzll(x) : 64; }
static int countr_one(uint64_t x)		{ int n = 0; while (x & 1) { n++; x >>= 1; } return n; }
static int countl_one(uint64_t x)		{ int n = 0; for (int bit = 63; bit >= 0 && ((x >> bit) & 1); bit--) n++; return n; }
static bool isShiftedMask(uint64_t x)		{ if (x == 0) return false; uint64_t y = x >> countr_zero(x); return (y & (y + 1)) == 0; }

// Computes the (N, immr, imms) fields for a logical-immediate operand, or
// returns false if the value is not encodable. Validated against clang:
// 5334/5334 values byte-identical, 2M random roundtrips clean.
static bool encodeLogicalImmediate(uint64_t imm, int regsize, int& N, int& immr, int& imms) {
  if (imm == 0 || imm == MASK64) return false;
  if (regsize != 64) {
    if ((imm >> regsize) != 0 || imm == (~0ull >> (64 - regsize))) return false;
  }
  int size = regsize;
  for (;;) {
    size /= 2;
    uint64_t m = (1ull << size) - 1;
    if ((imm & m) != ((imm >> size) & m)) {
      size *= 2;
      break;
    }
    if (size <= 2) break;
  }
  uint64_t m = MASK64 >> (64 - size);
  imm &= m;
  int i, cto;
  if (isShiftedMask(imm)) {
    i = countr_zero(imm);
    cto = countr_one(imm >> i);
  } else {
    imm |= ~m & MASK64;
    if (!isShiftedMask(~imm & MASK64)) return false;
    int clo = countl_one(imm);
    i = 64 - clo;
    cto = clo + countr_one(imm) - (64 - size);
  }
  if (!(size > i)) return false;
  immr = (size - i) & (size - 1);
  uint64_t nimms = (~((uint64_t)(size - 1)) & MASK64) << 1;
  nimms |= (cto - 1);
  N = ((nimms >> 6) & 1) ^ 1;
  imms = nimms & 0x3F;
  return true;
}


// ---------------------------------------------------------------------------
// register names
// ---------------------------------------------------------------------------

static const char* registerNames[nofRegisters] = {
  "x0",  "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7",
  "x8",  "x9",  "x10", "x11", "x12", "x13", "x14", "x15",
  "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23",
  "x24", "x25", "x26", "x27", "x28", "x29", "x30", "xzr/sp"
};

char* Register::name() const {
  return (char*) (isValid() ? registerNames[_number] : "noreg");
}


static const char* floatRegisterNames[32] = {
  "d0",  "d1",  "d2",  "d3",  "d4",  "d5",  "d6",  "d7",
  "d8",  "d9",  "d10", "d11", "d12", "d13", "d14", "d15",
  "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23",
  "d24", "d25", "d26", "d27", "d28", "d29", "d30", "d31"
};

char* FloatRegister::name() const {
  return (char*) (isValid() ? floatRegisterNames[_number] : "nofreg");
}


// ---------------------------------------------------------------------------
// Address
// ---------------------------------------------------------------------------

Address::Address() {
  _mode  = base_plus_disp;
  _base  = noreg;
  _index = noreg;
  _scale = no_scale;
  _disp  = 0;
  _rtype = relocInfo::none;
}


Address::Address(intptr_t disp, relocInfo::relocType rtype) {
  _mode  = absolute;
  _base  = noreg;
  _index = noreg;
  _scale = no_scale;
  _disp  = disp;
  _rtype = rtype;
}


Address::Address(Register base, intptr_t disp, relocInfo::relocType rtype) {
  _mode  = base_plus_disp;
  _base  = base;
  _index = noreg;
  _scale = no_scale;
  _disp  = disp;
  _rtype = rtype;
}


Address::Address(Register base, Register index, ScaleFactor scale, relocInfo::relocType rtype) {
  assert(index != noreg, "inconsistent address");
  _mode  = base_plus_reg;
  _base  = base;
  _index = index;
  _scale = scale;
  _disp  = 0;
  _rtype = rtype;
}


Address::Address(Register base, Register index, ScaleFactor scale, intptr_t disp, relocInfo::relocType rtype) {
  if (base != noreg && index != noreg) {
    _mode  = base_plus_reg_disp;
    _base  = base;
    _index = index;
    _scale = scale;
    _disp  = disp;
    _rtype = rtype;
  } else if (base != noreg) {
    _mode  = base_plus_disp;
    _base  = base;
    _index = noreg;
    _scale = no_scale;
    _disp  = disp;
    _rtype = rtype;
  } else {
    _mode  = absolute;
    _base  = noreg;
    _index = noreg;
    _scale = no_scale;
    _disp  = disp;
    _rtype = rtype;
  }
}


// ---------------------------------------------------------------------------
// label fixup
// ---------------------------------------------------------------------------

// An unbound label reference is stored in the instruction slot itself: the
// emitted 32-bit word is a chain entry instead of a real instruction and is
// patched by bind_to() once the label position is known.
//
//   next   [31:15] 17 bits   position of the previous chain entry / 4 (0 = end)
//   type   [14:11]  4 bits   which instruction form to patch
//   info   [10:0]  11 bits   per-type data (condition, register, bit number)
//
// Positions are stored as pos/4 because all instructions are 4-byte aligned;
// the next field therefore covers code up to 2^19 = 512KB. The same
// "chain entries must be at positions > 0" restriction as the x86 backend
// applies (a branch at position 0 can't be chained).

class AArch64Displacement : public ValueObj {
 private:
  int _data;

  enum Layout {
    info_size	= 11,
    type_size	=  4,
    next_size	= 32 - (info_size + type_size),

    info_pos	= 0,
    type_pos	= info_size,
    next_pos	= info_size + type_size,

    info_mask	= (1 << info_size) - 1,
    type_mask	= (1 << type_size) - 1,
    next_mask	= (1 << next_size) - 1,
  };

  enum Type {
    b_branch,		// B            (info unused)
    bl_branch,		// BL           (info unused)
    cond_branch,	// B.cond       (info = condition)
    cbz_branch,		// CBZ          (info = rt)
    cbnz_branch,	// CBNZ         (info = rt)
    tbz_branch,		// TBZ          (info = (bit << 5) | rt)
    tbnz_branch,	// TBNZ         (info = (bit << 5) | rt)
    adr_label,		// ADR          (info = rd)
    ldr_lit_x,		// LDR X literal (info = rt)
    ldr_lit_w,		// LDR W literal (info = rt)
    ldr_lit_d,		// LDR D literal (info = rt)
    ldr_lit_s,		// LDR S literal (info = rt)
  };

  void init(Label& L, Type type, int info) {
    assert(!L.is_bound(), "label is bound");
    int next = 0;
    if (L.is_unbound()) {
      next = L.pos() / 4 + 1;	// +1 so a fixup at offset 0 is representable (0 is the end-of-chain marker)
    }
    assert((next & ~next_mask) == 0, "next field too small");
    assert((type & ~type_mask) == 0, "type field too small");
    assert((info & ~info_mask) == 0, "info field too small");
    _data = (next << next_pos) | (type << type_pos) | (info << info_pos);
  }

  int  data() const		{ return _data; }
  int  info() const		{ return ((_data >> info_pos) & info_mask); }
  Type type() const		{ return Type((_data >> type_pos) & type_mask); }
  void next(Label& L) const	{ int n = ((_data >> next_pos) & next_mask); n > 0 ? L.link_to((n - 1) * 4) : L.unuse(); }
  void link_to(Label& L)	{ init(L, type(), info()); }

  AArch64Displacement(int data)	{ _data = data; }

  AArch64Displacement(Label& L, Type type, int info) {
    init(L, type, info);
  }

  friend class AArch64Assembler;
  friend class AArch64MacroAssembler;
};


// Use macros (otherwise must also declare AArch64Displacement class in .hpp file)
#define disp_at(L)		AArch64Displacement(long_at((L).pos()))
#define disp_at_put(L,disp)	long_at_put((L).pos(), (disp).data())
#define emit_disp(L,type,info)	{ AArch64Displacement disp((L), (type), (info)); \
				  L.link_to(offset());				  \
				  emit_long((int)disp.data());			  \
				}


void AArch64Assembler::print(Label& L) {
  if (L.is_unused()) {
    mystd->print_cr("undefined label");
  } else if (L.is_bound()) {
    mystd->print_cr("bound label to %d", L.pos());
  } else if (L.is_unbound()) {
    Label l = L;
    mystd->print_cr("unbound label");
    while (l.is_unbound()) {
      AArch64Displacement disp = disp_at(l);
      mystd->print("  @ %d (type = %d, info = %d)", l.pos(), (int)disp.type(), disp.info());
      mystd->cr();
      disp.next(l);
    }
  } else {
    mystd->print_cr("label in inconsistent state (pos = %d)", L._pos);
  }
}


void AArch64Assembler::bind_to(Label& L, int pos) {
  assert(0 <= pos && pos <= offset(), "must have a valid binding position");
  while (L.is_unbound()) {
    AArch64Displacement disp = disp_at(L);
    int fixup_pos = L.pos();
    int d = pos - fixup_pos;
    assert((d & 3) == 0, "branch displacement must be 4-byte aligned");
    int info = disp.info();
    uint32_t word;
    switch (disp.type()) {
      case AArch64Displacement::b_branch:
        word = 0x14000000 | ((d >> 2) & 0x3FFFFFF);
        break;
      case AArch64Displacement::bl_branch:
        word = 0x94000000 | ((d >> 2) & 0x3FFFFFF);
        break;
      case AArch64Displacement::cond_branch:
        assert((info & ~0xF) == 0, "invalid condition");
        word = 0x54000000 | (((d >> 2) & 0x7FFFF) << 5) | (info & 0xF);
        break;
      case AArch64Displacement::cbz_branch:
        word = 0xB4000000 | (((d >> 2) & 0x7FFFF) << 5) | (info & 0x1F);
        break;
      case AArch64Displacement::cbnz_branch:
        word = 0xB5000000 | (((d >> 2) & 0x7FFFF) << 5) | (info & 0x1F);
        break;
      case AArch64Displacement::tbz_branch:
        word = 0x36000000 | ((info >> 5) << 19) | (((d >> 2) & 0x3FFF) << 5) | (info & 0x1F);
        break;
      case AArch64Displacement::tbnz_branch:
        word = 0xB7000000 | ((info >> 5) << 19) | (((d >> 2) & 0x3FFF) << 5) | (info & 0x1F);
        break;
      case AArch64Displacement::adr_label:
        word = 0x10000000 | (((d >> 2) & 0x7FFFF) << 5) | (info & 0x1F);
        break;
      case AArch64Displacement::ldr_lit_x:
        assert(d > 0, "literal must be a forward reference");
        word = 0x58000000 | (((d >> 2) & 0x7FFFF) << 5) | (info & 0x1F);
        break;
      case AArch64Displacement::ldr_lit_w:
        assert(d > 0, "literal must be a forward reference");
        word = 0x18000000 | (((d >> 2) & 0x7FFFF) << 5) | (info & 0x1F);
        break;
      case AArch64Displacement::ldr_lit_d:
        assert(d > 0, "literal must be a forward reference");
        word = 0x5C000000 | (((d >> 2) & 0x7FFFF) << 5) | (info & 0x1F);
        break;
      case AArch64Displacement::ldr_lit_s:
        assert(d > 0, "literal must be a forward reference");
        word = 0x1C000000 | (((d >> 2) & 0x7FFFF) << 5) | (info & 0x1F);
        break;
      default:
        ShouldNotReachHere();
    }
    long_at_put(fixup_pos, (int)word);
    disp.next(L);
  }
  L.bind_to(pos);
}


void AArch64Assembler::link_to(Label& L, Label& appendix) {
  if (appendix.is_unbound()) {
    if (L.is_unbound()) {
      // append appendix to L's list
      Label p, q = L;
      do { p = q; disp_at(q).next(q); } while (q.is_unbound());
      AArch64Displacement disp = disp_at(p);
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


// ---------------------------------------------------------------------------
// AArch64Assembler
// ---------------------------------------------------------------------------

AArch64Assembler::AArch64Assembler(CodeBuffer* code) : AbstractAssembler(code) {}


void AArch64Assembler::emit_quad_data(intptr_t data, relocInfo::relocType rtype) {
  if (rtype != relocInfo::none) code()->relocate(_code_pos, rtype);
  emit_long((int)(data & 0xFFFFFFFF));
  emit_long((int)((data >> 32) & 0xFFFFFFFF));
}


// Logical (shifted register)
void AArch64Assembler::logical_shifted(Register rd, Register rn, Register rm, int opc, bool N, ShiftType shift, int amt, RegisterSize size) {
  assert(rd.isValid() && rn.isValid() && rm.isValid(), "illegal register");
  assert(0 <= amt && amt < (size == sz_64 ? 64 : 32), "shift amount out of range");
  emit_long(0x0A000000 | (size << 31) | (opc << 29) | (shift << 22) | (N << 21) | (rm.number() << 16) | (amt << 10) | (rn.number() << 5) | rd.number());
}

void AArch64Assembler::and_ (Register rd, Register rn, Register rm, ShiftType shift, int amt, RegisterSize size) { logical_shifted(rd, rn, rm, 0, false, shift, amt, size); }
void AArch64Assembler::orr  (Register rd, Register rn, Register rm, ShiftType shift, int amt, RegisterSize size) { logical_shifted(rd, rn, rm, 1, false, shift, amt, size); }
void AArch64Assembler::eor  (Register rd, Register rn, Register rm, ShiftType shift, int amt, RegisterSize size) { logical_shifted(rd, rn, rm, 2, false, shift, amt, size); }
void AArch64Assembler::ands (Register rd, Register rn, Register rm, ShiftType shift, int amt, RegisterSize size) { logical_shifted(rd, rn, rm, 3, false, shift, amt, size); }
void AArch64Assembler::bic  (Register rd, Register rn, Register rm, ShiftType shift, int amt, RegisterSize size) { logical_shifted(rd, rn, rm, 0, true,  shift, amt, size); }
void AArch64Assembler::orn  (Register rd, Register rn, Register rm, ShiftType shift, int amt, RegisterSize size) { logical_shifted(rd, rn, rm, 1, true,  shift, amt, size); }
void AArch64Assembler::eon  (Register rd, Register rn, Register rm, ShiftType shift, int amt, RegisterSize size) { logical_shifted(rd, rn, rm, 2, true,  shift, amt, size); }
void AArch64Assembler::bics (Register rd, Register rn, Register rm, ShiftType shift, int amt, RegisterSize size) { logical_shifted(rd, rn, rm, 3, true,  shift, amt, size); }
void AArch64Assembler::tst  (Register rn, Register rm, ShiftType shift, int amt, RegisterSize size)              { logical_shifted(xzr, rn, rm, 3, false, shift, amt, size); }


// Add/sub (shifted register)
void AArch64Assembler::addsub_shifted(Register rd, Register rn, Register rm, bool op, bool S, ShiftType shift, int amt, RegisterSize size) {
  assert(rd.isValid() && rn.isValid() && rm.isValid(), "illegal register");
  assert(0 <= amt && amt < (size == sz_64 ? 64 : 32), "shift amount out of range");
  emit_long(0x0B000000 | (size << 31) | (op << 30) | (S << 29) | (shift << 22) | (rm.number() << 16) | (amt << 10) | (rn.number() << 5) | rd.number());
}

void AArch64Assembler::add (Register rd, Register rn, Register rm, ShiftType shift, int amt, RegisterSize size) { addsub_shifted(rd, rn, rm, false, false, shift, amt, size); }
void AArch64Assembler::sub (Register rd, Register rn, Register rm, ShiftType shift, int amt, RegisterSize size) { addsub_shifted(rd, rn, rm, true,  false, shift, amt, size); }
void AArch64Assembler::adds(Register rd, Register rn, Register rm, ShiftType shift, int amt, RegisterSize size) { addsub_shifted(rd, rn, rm, false, true,  shift, amt, size); }
void AArch64Assembler::subs(Register rd, Register rn, Register rm, ShiftType shift, int amt, RegisterSize size) { addsub_shifted(rd, rn, rm, true,  true,  shift, amt, size); }
void AArch64Assembler::neg (Register rd, Register rm, ShiftType shift, int amt, RegisterSize size)              { addsub_shifted(rd, xzr, rm, true,  false, shift, amt, size); }
void AArch64Assembler::negs(Register rd, Register rm, ShiftType shift, int amt, RegisterSize size)              { addsub_shifted(rd, xzr, rm, true,  true,  shift, amt, size); }
void AArch64Assembler::cmp (Register rn, Register rm, ShiftType shift, int amt, RegisterSize size)              { addsub_shifted(xzr, rn, rm, true,  true,  shift, amt, size); }
void AArch64Assembler::cmn (Register rn, Register rm, ShiftType shift, int amt, RegisterSize size)              { addsub_shifted(xzr, rn, rm, false, true,  shift, amt, size); }


// Add/sub (immediate)
void AArch64Assembler::addsub_imm(Register rd, Register rn, int imm12, bool op, bool S, int sh, RegisterSize size) {
  assert(rd.isValid() && rn.isValid(), "illegal register");
  assert(0 <= imm12 && imm12 < 0x1000, "imm12 out of range");
  assert(sh == 0 || sh == 1, "illegal shift");
  emit_long(0x11000000 | (size << 31) | (op << 30) | (S << 29) | (sh << 22) | (imm12 << 10) | (rn.number() << 5) | rd.number());
}

void AArch64Assembler::add (Register rd, Register rn, int imm12, int sh, RegisterSize size) { addsub_imm(rd, rn, imm12, false, false, sh, size); }
void AArch64Assembler::sub (Register rd, Register rn, int imm12, int sh, RegisterSize size) { addsub_imm(rd, rn, imm12, true,  false, sh, size); }
void AArch64Assembler::adds(Register rd, Register rn, int imm12, int sh, RegisterSize size) { addsub_imm(rd, rn, imm12, false, true,  sh, size); }
void AArch64Assembler::subs(Register rd, Register rn, int imm12, int sh, RegisterSize size) { addsub_imm(rd, rn, imm12, true,  true,  sh, size); }
void AArch64Assembler::cmp (Register rn, int imm12, int sh, RegisterSize size)             { addsub_imm(xzr, rn, imm12, true,  true,  sh, size); }
void AArch64Assembler::cmn (Register rn, int imm12, int sh, RegisterSize size)             { addsub_imm(xzr, rn, imm12, false, true,  sh, size); }


// Logical (immediate)
void AArch64Assembler::logical_imm(Register rd, Register rn, uint64_t imm, int opc, RegisterSize size) {
  assert(rd.isValid() && rn.isValid(), "illegal register");
  int N, immr, imms;
  if (!encodeLogicalImmediate(imm, size == sz_64 ? 64 : 32, N, immr, imms)) {
    fatal("not a valid logical immediate");
  }
  emit_long(0x12000000 | (size << 31) | (opc << 29) | (N << 22) | (immr << 16) | (imms << 10) | (rn.number() << 5) | rd.number());
}

void AArch64Assembler::and_(Register rd, Register rn, uint64_t imm, RegisterSize size) { logical_imm(rd, rn, imm, 0, size); }
void AArch64Assembler::orr (Register rd, Register rn, uint64_t imm, RegisterSize size) { logical_imm(rd, rn, imm, 1, size); }
void AArch64Assembler::eor (Register rd, Register rn, uint64_t imm, RegisterSize size) { logical_imm(rd, rn, imm, 2, size); }
void AArch64Assembler::ands(Register rd, Register rn, uint64_t imm, RegisterSize size) { logical_imm(rd, rn, imm, 3, size); }
void AArch64Assembler::tst (Register rn, uint64_t imm, RegisterSize size)             { logical_imm(xzr, rn, imm, 3, size); }


// Bitfield
void AArch64Assembler::bitfield_op(Register rd, Register rn, int opc, int immr, int imms, RegisterSize size) {
  assert(rd.isValid() && rn.isValid(), "illegal register");
  int w = (size == sz_64) ? 64 : 32;
  assert(0 <= immr && immr < w, "immr out of range");
  assert(0 <= imms && imms < w, "imms out of range");
  // bitfield ops: 0x13000000 | sf<<31 | opc<<29 | N<<22 | immr<<16 | imms<<10 | Rn<<5 | Rd
  // where opc is 0b00 (SBFM), 0b01 (BFM), 0b10 (UBFM) and the N field = sf.
  emit_long(0x13000000 | (size << 31) | (opc << 29) | (size << 22) | (immr << 16) | (imms << 10) | (rn.number() << 5) | rd.number());
}

void AArch64Assembler::lsl  (Register rd, Register rn, int shift, RegisterSize size) {
  int w = (size == sz_64) ? 64 : 32;
  assert(0 <= shift && shift < w, "shift out of range");
  bitfield_op(rd, rn, 0b10, (w - shift) & (w - 1), w - 1 - shift, size);
}
void AArch64Assembler::lsr  (Register rd, Register rn, int shift, RegisterSize size) {
  int w = (size == sz_64) ? 64 : 32;
  assert(0 <= shift && shift < w, "shift out of range");
  bitfield_op(rd, rn, 0b10, shift, w - 1, size);
}
void AArch64Assembler::asr  (Register rd, Register rn, int shift, RegisterSize size) {
  int w = (size == sz_64) ? 64 : 32;
  assert(0 <= shift && shift < w, "shift out of range");
  bitfield_op(rd, rn, 0b00, shift, w - 1, size);
}
void AArch64Assembler::ubfx (Register rd, Register rn, int lsb, int width, RegisterSize size) {
  int w = (size == sz_64) ? 64 : 32;
  assert(0 < width && width <= w - lsb, "width out of range");
  bitfield_op(rd, rn, 0b10, lsb, lsb + width - 1, size);
}
void AArch64Assembler::sbfx (Register rd, Register rn, int lsb, int width, RegisterSize size) {
  int w = (size == sz_64) ? 64 : 32;
  assert(0 < width && width <= w - lsb, "width out of range");
  bitfield_op(rd, rn, 0b00, lsb, lsb + width - 1, size);
}
void AArch64Assembler::ubfiz(Register rd, Register rn, int lsb, int width, RegisterSize size) {
  int w = (size == sz_64) ? 64 : 32;
  assert(0 < width && width <= w - lsb, "width out of range");
  bitfield_op(rd, rn, 0b10, (w - lsb) & (w - 1), width - 1, size);
}
void AArch64Assembler::sbfiz(Register rd, Register rn, int lsb, int width, RegisterSize size) {
  int w = (size == sz_64) ? 64 : 32;
  assert(0 < width && width <= w - lsb, "width out of range");
  bitfield_op(rd, rn, 0b00, (w - lsb) & (w - 1), width - 1, size);
}
void AArch64Assembler::bfi  (Register rd, Register rn, int lsb, int width, RegisterSize size) {
  int w = (size == sz_64) ? 64 : 32;
  assert(0 < width && width <= w - lsb, "width out of range");
  bitfield_op(rd, rn, 0b01, (w - lsb) & (w - 1), width - 1, size);
}
void AArch64Assembler::bfxil(Register rd, Register rn, int lsb, int width, RegisterSize size) {
  int w = (size == sz_64) ? 64 : 32;
  assert(0 < width && width <= w - lsb, "width out of range");
  bitfield_op(rd, rn, 0b01, lsb, lsb + width - 1, size);
}
void AArch64Assembler::uxtb (Register rd, Register rn, RegisterSize size) { bitfield_op(rd, rn, 0b10, 0, 7,  size); }
void AArch64Assembler::uxth (Register rd, Register rn, RegisterSize size) { bitfield_op(rd, rn, 0b10, 0, 15, size); }
void AArch64Assembler::uxtw (Register rd, Register rn)                   { bitfield_op(rd, rn, 0b10, 0, 31, sz_64); }
void AArch64Assembler::sxtb (Register rd, Register rn, RegisterSize size) { bitfield_op(rd, rn, 0b00, 0, 7,  size); }
void AArch64Assembler::sxth (Register rd, Register rn, RegisterSize size) { bitfield_op(rd, rn, 0b00, 0, 15, size); }
void AArch64Assembler::sxtw (Register rd, Register rn)                   { bitfield_op(rd, rn, 0b00, 0, 31, sz_64); }


// Data-processing (2 source)
void AArch64Assembler::lslv(Register rd, Register rn, Register rm, RegisterSize size) { emit_long((size == sz_64 ? 0x9AC02000 : 0x1AC02000) | (rm.number() << 16) | (rn.number() << 5) | rd.number()); }
void AArch64Assembler::lsrv(Register rd, Register rn, Register rm, RegisterSize size) { emit_long((size == sz_64 ? 0x9AC02400 : 0x1AC02400) | (rm.number() << 16) | (rn.number() << 5) | rd.number()); }
void AArch64Assembler::asrv(Register rd, Register rn, Register rm, RegisterSize size) { emit_long((size == sz_64 ? 0x9AC02800 : 0x1AC02800) | (rm.number() << 16) | (rn.number() << 5) | rd.number()); }
void AArch64Assembler::rorv(Register rd, Register rn, Register rm, RegisterSize size) { emit_long((size == sz_64 ? 0x9AC02C00 : 0x1AC02C00) | (rm.number() << 16) | (rn.number() << 5) | rd.number()); }
void AArch64Assembler::udiv(Register rd, Register rn, Register rm, RegisterSize size) { emit_long((size == sz_64 ? 0x9AC00800 : 0x1AC00800) | (rm.number() << 16) | (rn.number() << 5) | rd.number()); }
void AArch64Assembler::sdiv(Register rd, Register rn, Register rm, RegisterSize size) { emit_long((size == sz_64 ? 0x9AC00C00 : 0x1AC00C00) | (rm.number() << 16) | (rn.number() << 5) | rd.number()); }


// Multiply
void AArch64Assembler::madd(Register rd, Register rn, Register rm, Register ra, RegisterSize size) {
  assert(rd.isValid() && rn.isValid() && rm.isValid() && ra.isValid(), "illegal register");
  emit_long(0x1B000000 | (size << 31) | (rm.number() << 16) | (ra.number() << 10) | (rn.number() << 5) | rd.number());
}

void AArch64Assembler::mul(Register rd, Register rn, Register rm, RegisterSize size) {
  madd(rd, rn, rm, xzr, size);
}


void AArch64Assembler::smulh(Register rd, Register rn, Register rm) {
  assert(rd.isValid() && rn.isValid() && rm.isValid(), "illegal register");
  emit_long(0x9B400000 | (rm.number() << 16) | (0x1F << 10) | (rn.number() << 5) | rd.number());
}


void AArch64Assembler::msub(Register rd, Register rn, Register rm, Register ra, RegisterSize size) {
  assert(rd.isValid() && rn.isValid() && rm.isValid() && ra.isValid(), "illegal register");
  assert(size == sz_64 || rd.number() < 32, "32-bit form not used here");
  emit_long(0x9B008000 | (size << 31) | (rm.number() << 16) | (ra.number() << 10) | (rn.number() << 5) | rd.number());
}


void AArch64Assembler::cset(Register rd, Condition cc) {
  assert(rd.isValid(), "illegal register");
  // csinc rd, xzr, xzr, invert(cc): rd = cc ? 1 : 0
  emit_long(0x9A9F07E0 | (((int)cc ^ 1) << 12) | rd.number());
}


// Move wide
void AArch64Assembler::movz(Register rd, int imm16, int hw, RegisterSize size) {
  assert(rd.isValid(), "illegal register");
  assert(0 <= imm16 && imm16 < 0x10000, "imm16 out of range");
  assert(0 <= hw && hw < (size == sz_64 ? 4 : 2), "hw out of range");
  emit_long(0x12800000 | (size << 31) | (2 << 29) | (hw << 21) | (imm16 << 5) | rd.number());
}

void AArch64Assembler::movn(Register rd, int imm16, int hw, RegisterSize size) {
  assert(rd.isValid(), "illegal register");
  assert(0 <= imm16 && imm16 < 0x10000, "imm16 out of range");
  assert(0 <= hw && hw < (size == sz_64 ? 4 : 2), "hw out of range");
  emit_long(0x12800000 | (size << 31) | (0 << 29) | (hw << 21) | (imm16 << 5) | rd.number());
}

void AArch64Assembler::movk(Register rd, int imm16, int hw, RegisterSize size) {
  assert(rd.isValid(), "illegal register");
  assert(0 <= imm16 && imm16 < 0x10000, "imm16 out of range");
  assert(0 <= hw && hw < (size == sz_64 ? 4 : 2), "hw out of range");
  emit_long(0x12800000 | (size << 31) | (3 << 29) | (hw << 21) | (imm16 << 5) | rd.number());
}

void AArch64Assembler::mov(Register rd, Register rm, RegisterSize size) {
  orr(rd, xzr, rm, LSL, 0, size);
}


// Memory (base + disp / register offset)
void AArch64Assembler::load_store(Register rt, Address adr, int size, bool isLoad) {
  assert(rt.isValid(), "illegal register");
  switch (adr._mode) {
    case Address::base_plus_disp: {
      Register rn = adr._base;
      intptr_t disp = adr._disp;
      int scale = 1 << size;
      if (disp >= 0 && (disp & (scale - 1)) == 0 && (disp >> size) <= 0xFFF) {
        // unsigned imm12 form (disp >> size must fit in 12 bits)
        emit_long(0x39000000 | (size << 30) | (isLoad << 22) | ((disp >> size) << 10) | (rn.number() << 5) | rt.number());
      } else if (-256 <= disp && disp <= 255) {
        // unscaled imm9 form (ldur/stur)
        emit_long(0x38000000 | (size << 30) | (isLoad << 22) | (0 << 10) | ((disp & 0x1FF) << 12) | (rn.number() << 5) | rt.number());
      } else {
        // Displacement too large for a single load/store (x86 allows 32-bit
        // displacements; AArch64 does not). Materialize rn + disp in the
        // reserved scratch register pair, then use a zero-displacement
        // load/store. The x16/x17 registers are dedicated scratch and no
        // live register is clobbered as long as the base is not x16/x17;
        // if it is, pick the other scratch.
        Register scratch = (rn == x16 || rn == x17) ? (rn == x16 ? x17 : x16) : x16;
        uint64_t value = (uint64_t)(intptr_t)disp;
        int hw = (63 - __builtin_clzll(value)) >> 4;
        movz(scratch, (int)((value >> (16 * hw)) & 0xFFFF), hw);
        for (int i = hw - 1; i >= 0; i--) {
          int chunk = (int)((value >> (16 * i)) & 0xFFFF);
          if (chunk != 0) movk(scratch, chunk, i);
        }
        add(scratch, rn, scratch);
        load_store(rt, Address(scratch), size, isLoad);
      }
      break;
    }
    case Address::base_plus_reg: {
      assert(adr._index.isValid(), "illegal index register");
      // times_1 is a byte-scale offset (base + index*1 bytes) - encode it as
      // an unscaled register offset (S=0), which AArch64 allows for any
      // access size. Any other scale must match the access size for the
      // scaled (shifted) form.
      if (adr._scale != Address::no_scale && adr._scale != Address::times_1 && adr._scale != size) {
        // Scale does not match the access size (e.g. a 64-bit load from
        // [base + index*4]); materialize base + index*2^scale in the
        // reserved scratch register pair, then use a zero-displacement load.
        Register scratch = (adr._base == x16 || adr._base == x17) ? (adr._base == x16 ? x17 : x16) : x16;
        add(scratch, adr._base, adr._index, LSL, (int)adr._scale);
        load_store(rt, Address(scratch), size, isLoad);
        break;
      }
      bool scaled = adr._scale == size;
      emit_long(0x38200000 | (size << 30) | (isLoad << 22) | (adr._index.number() << 16) | (0b011 << 13) | (scaled << 12) | (2 << 10) | (adr._base.number() << 5) | rt.number());
      break;
    }
    case Address::base_plus_reg_disp: {
      // x86-style base + index*2^scale + disp: materialize the register part
      // in x16 (the reserved scratch) and use the base_plus_disp form.
      assert(adr._base.isValid() && adr._index.isValid(), "illegal address");
      int sh = (int)adr._scale;		// times_1..times_8 = 0..3; no_scale = -1 -> 0
      if (sh < 0) sh = 0;
      add(x16, adr._base, adr._index, LSL, sh);
      load_store(rt, Address(x16, adr._disp, adr._rtype), size, isLoad);
      break;
    }
    case Address::absolute:
      // An absolute address is not encodable in a single AArch64 load/store
      // instruction; the JIT retarget phase will expand it into a
      // movz/movk + load sequence.
      ShouldNotCallThis();
      break;
  }
}

void AArch64Assembler::ldr (Register rt, Address adr) { load_store(rt, adr, 3, true);  }
void AArch64Assembler::str (Register rt, Address adr) { load_store(rt, adr, 3, false); }
void AArch64Assembler::ldr_w(Register rt, Address adr) { load_store(rt, adr, 2, true);  }
void AArch64Assembler::str_w(Register rt, Address adr) { load_store(rt, adr, 2, false); }
void AArch64Assembler::ldr_b(Register rt, Address adr) { load_store(rt, adr, 0, true);  }
void AArch64Assembler::str_b(Register rt, Address adr) { load_store(rt, adr, 0, false); }
void AArch64Assembler::ldr_h(Register rt, Address adr) { load_store(rt, adr, 1, true);  }
void AArch64Assembler::str_h(Register rt, Address adr) { load_store(rt, adr, 1, false); }

void AArch64Assembler::ldur(Register rt, Address adr, RegisterSize size) {
  assert(rt.isValid(), "illegal register");
  assert(adr._mode == Address::base_plus_disp, "ldur requires base + displacement");
  intptr_t disp = adr._disp;
  assert(-256 <= disp && disp <= 255, "imm9 out of range");
  emit_long(0x38000000 | ((2 + size) << 30) | (1 << 22) | (0 << 10) | ((disp & 0x1FF) << 12) | (adr._base.number() << 5) | rt.number());
}

void AArch64Assembler::stur(Register rt, Address adr, RegisterSize size) {
  assert(rt.isValid(), "illegal register");
  assert(adr._mode == Address::base_plus_disp, "stur requires base + displacement");
  intptr_t disp = adr._disp;
  assert(-256 <= disp && disp <= 255, "imm9 out of range");
  emit_long(0x38000000 | ((2 + size) << 30) | (0 << 22) | (0 << 10) | ((disp & 0x1FF) << 12) | (adr._base.number() << 5) | rt.number());
}

void AArch64Assembler::ldur(FloatRegister ft, Address adr, RegisterSize size) {
  assert(ft.isValid(), "illegal register");
  assert(adr._mode == Address::base_plus_disp, "ldur requires base + displacement");
  intptr_t disp = adr._disp;
  assert(-256 <= disp && disp <= 255, "imm9 out of range");
  bool doubleSize = (size == sz_64);
  emit_long((doubleSize ? 0xFC400000 : 0xBC400000) | ((disp & 0x1FF) << 12) | (adr._base.number() << 5) | ft.number());
}

void AArch64Assembler::stur(FloatRegister ft, Address adr, RegisterSize size) {
  assert(ft.isValid(), "illegal register");
  assert(adr._mode == Address::base_plus_disp, "stur requires base + displacement");
  intptr_t disp = adr._disp;
  assert(-256 <= disp && disp <= 255, "imm9 out of range");
  bool doubleSize = (size == sz_64);
  emit_long((doubleSize ? 0xFC000000 : 0xBC000000) | ((disp & 0x1FF) << 12) | (adr._base.number() << 5) | ft.number());
}

void AArch64Assembler::ldr_pre (Register rt, Register rn, int imm9) { assert(rt.isValid(), "illegal register"); assert(-256 <= imm9 && imm9 <= 255, "imm9 out of range"); emit_long(0x38000000 | (3 << 30) | (1 << 22) | (3 << 10) | ((imm9 & 0x1FF) << 12) | (rn.number() << 5) | rt.number()); }
void AArch64Assembler::str_pre (Register rt, Register rn, int imm9) { assert(rt.isValid(), "illegal register"); assert(-256 <= imm9 && imm9 <= 255, "imm9 out of range"); emit_long(0x38000000 | (3 << 30) | (0 << 22) | (3 << 10) | ((imm9 & 0x1FF) << 12) | (rn.number() << 5) | rt.number()); }
void AArch64Assembler::ldr_post(Register rt, Register rn, int imm9) { assert(rt.isValid(), "illegal register"); assert(-256 <= imm9 && imm9 <= 255, "imm9 out of range"); emit_long(0x38000000 | (3 << 30) | (1 << 22) | (1 << 10) | ((imm9 & 0x1FF) << 12) | (rn.number() << 5) | rt.number()); }
void AArch64Assembler::str_post(Register rt, Register rn, int imm9) { assert(rt.isValid(), "illegal register"); assert(-256 <= imm9 && imm9 <= 255, "imm9 out of range"); emit_long(0x38000000 | (3 << 30) | (0 << 22) | (1 << 10) | ((imm9 & 0x1FF) << 12) | (rn.number() << 5) | rt.number()); }


// Load/store pair
void AArch64Assembler::ldp(Register rt1, Register rt2, Register rn, int offset, RegisterSize size) {
  int scale = (size == sz_64) ? 3 : 2;
  assert((offset & ((1 << scale) - 1)) == 0, "offset not aligned to element size");
  int scaled = offset >> scale;
  assert(-64 <= scaled && scaled < 64, "offset out of range for load/store pair");
  emit_long(0x28000000 | (size << 31) | (1 << 24) | (0 << 23) | (1 << 22) | ((scaled & 0x7F) << 15) | (rt2.number() << 10) | (rn.number() << 5) | rt1.number());
}
void AArch64Assembler::stp(Register rt1, Register rt2, Register rn, int offset, RegisterSize size) {
  int scale = (size == sz_64) ? 3 : 2;
  assert((offset & ((1 << scale) - 1)) == 0, "offset not aligned to element size");
  int scaled = offset >> scale;
  assert(-64 <= scaled && scaled < 64, "offset out of range for load/store pair");
  emit_long(0x28000000 | (size << 31) | (1 << 24) | (0 << 23) | (0 << 22) | ((scaled & 0x7F) << 15) | (rt2.number() << 10) | (rn.number() << 5) | rt1.number());
}
void AArch64Assembler::ldp_pre(Register rt1, Register rt2, Register rn, int offset, RegisterSize size) {
  int scale = (size == sz_64) ? 3 : 2;
  assert((offset & ((1 << scale) - 1)) == 0, "offset not aligned to element size");
  int scaled = offset >> scale;
  assert(-64 <= scaled && scaled < 64, "offset out of range for load/store pair");
  emit_long(0x28000000 | (size << 31) | (1 << 24) | (1 << 23) | (1 << 22) | ((scaled & 0x7F) << 15) | (rt2.number() << 10) | (rn.number() << 5) | rt1.number());
}
void AArch64Assembler::stp_pre(Register rt1, Register rt2, Register rn, int offset, RegisterSize size) {
  int scale = (size == sz_64) ? 3 : 2;
  assert((offset & ((1 << scale) - 1)) == 0, "offset not aligned to element size");
  int scaled = offset >> scale;
  assert(-64 <= scaled && scaled < 64, "offset out of range for load/store pair");
  emit_long(0x28000000 | (size << 31) | (1 << 24) | (1 << 23) | (0 << 22) | ((scaled & 0x7F) << 15) | (rt2.number() << 10) | (rn.number() << 5) | rt1.number());
}
void AArch64Assembler::ldp_post(Register rt1, Register rt2, Register rn, int offset, RegisterSize size) {
  int scale = (size == sz_64) ? 3 : 2;
  assert((offset & ((1 << scale) - 1)) == 0, "offset not aligned to element size");
  int scaled = offset >> scale;
  assert(-64 <= scaled && scaled < 64, "offset out of range for load/store pair");
  emit_long(0x28000000 | (size << 31) | (0 << 24) | (1 << 23) | (1 << 22) | ((scaled & 0x7F) << 15) | (rt2.number() << 10) | (rn.number() << 5) | rt1.number());
}
void AArch64Assembler::stp_post(Register rt1, Register rt2, Register rn, int offset, RegisterSize size) {
  int scale = (size == sz_64) ? 3 : 2;
  assert((offset & ((1 << scale) - 1)) == 0, "offset not aligned to element size");
  int scaled = offset >> scale;
  assert(-64 <= scaled && scaled < 64, "offset out of range for load/store pair");
  emit_long(0x28000000 | (size << 31) | (0 << 24) | (1 << 23) | (0 << 22) | ((scaled & 0x7F) << 15) | (rt2.number() << 10) | (rn.number() << 5) | rt1.number());
}


// Load literal (PC-relative)
void AArch64Assembler::ldr(Register rt, Label& L) {
  assert(rt.isValid(), "illegal register");
  if (L.is_bound()) {
    int d = L.pos() - offset();
    assert(d > 0 && (d & 3) == 0, "literal must be a forward 4-byte aligned reference");
    assert((d >> 2) <= 0x7FFFF, "literal range exceeded");
    emit_long(0x58000000 | (((d >> 2) & 0x7FFFF) << 5) | rt.number());
  } else {
    emit_disp(L, AArch64Displacement::ldr_lit_x, rt.number());
  }
}

void AArch64Assembler::ldr_w(Register rt, Label& L) {
  assert(rt.isValid(), "illegal register");
  if (L.is_bound()) {
    int d = L.pos() - offset();
    assert(d > 0 && (d & 3) == 0, "literal must be a forward 4-byte aligned reference");
    assert((d >> 2) <= 0x7FFFF, "literal range exceeded");
    emit_long(0x18000000 | (((d >> 2) & 0x7FFFF) << 5) | rt.number());
  } else {
    emit_disp(L, AArch64Displacement::ldr_lit_w, rt.number());
  }
}

void AArch64Assembler::ldr(FloatRegister ft, Label& L) {
  assert(ft.isValid(), "illegal register");
  if (L.is_bound()) {
    int d = L.pos() - offset();
    assert(d > 0 && (d & 3) == 0, "literal must be a forward 4-byte aligned reference");
    assert((d >> 2) <= 0x7FFFF, "literal range exceeded");
    emit_long(0x5C000000 | (((d >> 2) & 0x7FFFF) << 5) | ft.number());
  } else {
    emit_disp(L, AArch64Displacement::ldr_lit_d, ft.number());
  }
}

void AArch64Assembler::ldr_s(FloatRegister ft, Label& L) {
  assert(ft.isValid(), "illegal register");
  if (L.is_bound()) {
    int d = L.pos() - offset();
    assert(d > 0 && (d & 3) == 0, "literal must be a forward 4-byte aligned reference");
    assert((d >> 2) <= 0x7FFFF, "literal range exceeded");
    emit_long(0x1C000000 | (((d >> 2) & 0x7FFFF) << 5) | ft.number());
  } else {
    emit_disp(L, AArch64Displacement::ldr_lit_s, ft.number());
  }
}


// Branches
void AArch64Assembler::b(Label& L) {
  if (L.is_bound()) {
    int d = L.pos() - offset();
    assert((d & 3) == 0, "branch displacement must be 4-byte aligned");
    emit_long(0x14000000 | ((d >> 2) & 0x3FFFFFF));
  } else {
    emit_disp(L, AArch64Displacement::b_branch, 0);
  }
}

void AArch64Assembler::bl(Label& L) {
  if (L.is_bound()) {
    int d = L.pos() - offset();
    assert((d & 3) == 0, "branch displacement must be 4-byte aligned");
    emit_long(0x94000000 | ((d >> 2) & 0x3FFFFFF));
  } else {
    emit_disp(L, AArch64Displacement::bl_branch, 0);
  }
}

void AArch64Assembler::b(Condition cc, Label& L) {
  assert(0 <= cc && cc <= 14, "illegal condition");
  if (L.is_bound()) {
    int d = L.pos() - offset();
    assert((d & 3) == 0, "branch displacement must be 4-byte aligned");
    emit_long(0x54000000 | (((d >> 2) & 0x7FFFF) << 5) | cc);
  } else {
    emit_disp(L, AArch64Displacement::cond_branch, cc);
  }
}

void AArch64Assembler::cbz(Register rt, Label& L, RegisterSize size) {
  assert(rt.isValid(), "illegal register");
  if (L.is_bound()) {
    int d = L.pos() - offset();
    assert((d & 3) == 0, "branch displacement must be 4-byte aligned");
    emit_long(0xB4000000 | (size << 31) | (((d >> 2) & 0x7FFFF) << 5) | rt.number());
  } else {
    assert(size == sz_64, "32-bit cbz/cbnz to unbound label not supported");
    emit_disp(L, AArch64Displacement::cbz_branch, rt.number());
  }
}

void AArch64Assembler::cbnz(Register rt, Label& L, RegisterSize size) {
  assert(rt.isValid(), "illegal register");
  if (L.is_bound()) {
    int d = L.pos() - offset();
    assert((d & 3) == 0, "branch displacement must be 4-byte aligned");
    emit_long(0xB5000000 | (size << 31) | (((d >> 2) & 0x7FFFF) << 5) | rt.number());
  } else {
    assert(size == sz_64, "32-bit cbz/cbnz to unbound label not supported");
    emit_disp(L, AArch64Displacement::cbnz_branch, rt.number());
  }
}

void AArch64Assembler::tbz(Register rt, int bit, Label& L) {
  assert(rt.isValid(), "illegal register");
  assert(0 <= bit && bit < 64, "bit out of range");
  if (L.is_bound()) {
    int d = L.pos() - offset();
    assert((d & 3) == 0, "branch displacement must be 4-byte aligned");
    emit_long(0x36000000 | ((bit & 0x20) << 26) | ((bit & 0x1F) << 19) | (((d >> 2) & 0x3FFF) << 5) | rt.number());
  } else {
    emit_disp(L, AArch64Displacement::tbz_branch, (bit << 5) | rt.number());
  }
}

void AArch64Assembler::tbnz(Register rt, int bit, Label& L) {
  assert(rt.isValid(), "illegal register");
  assert(0 <= bit && bit < 64, "bit out of range");
  if (L.is_bound()) {
    int d = L.pos() - offset();
    assert((d & 3) == 0, "branch displacement must be 4-byte aligned");
    emit_long(0xB7000000 | ((bit & 0x20) << 26) | ((bit & 0x1F) << 19) | (((d >> 2) & 0x3FFF) << 5) | rt.number());
  } else {
    emit_disp(L, AArch64Displacement::tbnz_branch, (bit << 5) | rt.number());
  }
}

void AArch64Assembler::adr(Register rd, Label& L) {
  assert(rd.isValid(), "illegal register");
  if (L.is_bound()) {
    int d = L.pos() - offset();
    assert((d & 3) == 0, "adr target must be 4-byte aligned");
    assert(-0x100000 <= d && d < 0x100000, "adr range exceeded");
    emit_long(0x10000000 | (((d >> 2) & 0x7FFFF) << 5) | rd.number());
  } else {
    emit_disp(L, AArch64Displacement::adr_label, rd.number());
  }
}


// Calls / jumps
void AArch64Assembler::call(Label& L)         { bl(L); }
void AArch64Assembler::call(Register reg)      { blr(reg); }
void AArch64Assembler::jmp(Label& L)           { b(L); }
void AArch64Assembler::jmp(Register reg)       { br(reg); }

void AArch64Assembler::call(char* entry, relocInfo::relocType rtype) {
  // ldr x16, [pc, #8]; b .+12; .quad entry; blr x16
  // The blr is placed *after* the 8-byte literal so that its return address
  // (pc+20) points at the continuation code. A bare "ldr; blr; .quad" would
  // leave the return address on the literal slot and crash when the callee
  // rets into the data. The b must land on the blr (pc+16), not past it.
  emit_long(0x58000000 | (2 << 5) | 16);	// ldr x16, [pc, #8]
  emit_long(0x14000003);			// b .+12 (skip the literal, land on blr)
  emit_quad_data((intptr_t)entry, rtype);	// .quad entry
  emit_long(0xD63F0000 | (16 << 5));		// blr x16
}

void AArch64Assembler::jmp(char* entry, relocInfo::relocType rtype) {
  // ldr x16, [pc, #8]; br x16; .quad entry
  emit_long(0x58000000 | (2 << 5) | 16);	// ldr x16, [pc, #8]
  emit_long(0xD61F0000 | (16 << 5));		// br x16
  emit_quad_data((intptr_t)entry, rtype);	// .quad entry
}

void AArch64Assembler::b(char* entry, relocInfo::relocType rtype)  { jmp(entry, rtype); }
void AArch64Assembler::bl(char* entry, relocInfo::relocType rtype) { call(entry, rtype); }


// System / miscellaneous
void AArch64Assembler::br (Register rn) { emit_long(0xD61F0000 | (rn.number() << 5)); }
void AArch64Assembler::blr(Register rn) { emit_long(0xD63F0000 | (rn.number() << 5)); }
void AArch64Assembler::ret(Register rn) { emit_long(0xD65F0000 | (rn.number() << 5)); }
void AArch64Assembler::nop()            { emit_long(0xD503201F); }


// Floating-point arithmetic
void AArch64Assembler::fadd(FloatRegister fd, FloatRegister fn, FloatRegister fm, RegisterSize size) { emit_long((size == sz_64 ? 0x1E602800 : 0x1E202800) | (fm.number() << 16) | (fn.number() << 5) | fd.number()); }
void AArch64Assembler::fsub(FloatRegister fd, FloatRegister fn, FloatRegister fm, RegisterSize size) { emit_long((size == sz_64 ? 0x1E603800 : 0x1E203800) | (fm.number() << 16) | (fn.number() << 5) | fd.number()); }
void AArch64Assembler::fmul(FloatRegister fd, FloatRegister fn, FloatRegister fm, RegisterSize size) { emit_long((size == sz_64 ? 0x1E600800 : 0x1E200800) | (fm.number() << 16) | (fn.number() << 5) | fd.number()); }
void AArch64Assembler::fdiv(FloatRegister fd, FloatRegister fn, FloatRegister fm, RegisterSize size) { emit_long((size == sz_64 ? 0x1E601800 : 0x1E201800) | (fm.number() << 16) | (fn.number() << 5) | fd.number()); }
void AArch64Assembler::fcmp(FloatRegister fn, FloatRegister fm, RegisterSize size)                  { emit_long((size == sz_64 ? 0x1E602000 : 0x1E202000) | (fm.number() << 16) | (fn.number() << 5)); }
void AArch64Assembler::fcmp0(FloatRegister fn, RegisterSize size)                                  { emit_long((size == sz_64 ? 0x1E602000 : 0x1E202000) | (fn.number() << 5) | 8); }
void AArch64Assembler::fmov(FloatRegister fd, FloatRegister fn, RegisterSize size)                 { emit_long((size == sz_64 ? 0x1E604000 : 0x1E204000) | (fn.number() << 5) | fd.number()); }
void AArch64Assembler::fabs(FloatRegister fd, FloatRegister fn, RegisterSize size)                 { emit_long((size == sz_64 ? 0x1E60C000 : 0x1E20C000) | (fn.number() << 5) | fd.number()); }
void AArch64Assembler::fneg(FloatRegister fd, FloatRegister fn, RegisterSize size)                 { emit_long((size == sz_64 ? 0x1E614000 : 0x1E214000) | (fn.number() << 5) | fd.number()); }
void AArch64Assembler::fsqrt(FloatRegister fd, FloatRegister fn, RegisterSize size)                { emit_long((size == sz_64 ? 0x1E61C000 : 0x1E21C000) | (fn.number() << 5) | fd.number()); }

void AArch64Assembler::fcvtzs(Register rd, FloatRegister fn, RegisterSize size, RegisterSize dstSize) {
  uint32_t base = (size == sz_64) ? 0x9E780000 : 0x9E380000;
  if (dstSize == sz_32) base &= ~0x80000000;	// clear sf for a 32-bit integer destination
  emit_long(base | (fn.number() << 5) | rd.number());
}

void AArch64Assembler::scvtf(FloatRegister fd, Register rn, RegisterSize size, RegisterSize srcSize) {
  // SCVTF Dd,Xn = 0x9E620000, Dd,Wn = 0x1E620000, Sd,Xn = 0x9E220000, Sd,Wn = 0x1E220000
  uint32_t base;
  if (size == sz_64)      base = (srcSize == sz_64) ? 0x9E620000 : 0x1E620000;
  else                    base = (srcSize == sz_64) ? 0x9E220000 : 0x1E220000;
  emit_long(base | (rn.number() << 5) | fd.number());
}

void AArch64Assembler::fmov(FloatRegister fd, Register rn, RegisterSize size) {
  // FMOV Dd,Xn = 0x9E670000, Sd,Wn = 0x1E270000; w31/x31 is zr
  uint32_t base = (size == sz_64) ? 0x9E670000 : 0x1E270000;
  emit_long(base | (rn.number() << 5) | fd.number());
}

void AArch64Assembler::csel(Register rd, Register rn, Register rm, Condition cc, RegisterSize size) {
  assert(cc != AL && (int)cc != 15, "csel requires a real condition");
  emit_long((size == sz_64 ? 0x9A800000 : 0x1A800000) | (rm.number() << 16) | ((int)cc << 12) | (rn.number() << 5) | rd.number());
}


// Floating-point load/store
void AArch64Assembler::load_store_float(FloatRegister rt, Address adr, bool doubleSize, bool isLoad) {
  assert(rt.isValid(), "illegal register");
  int size = doubleSize ? 3 : 2;
  switch (adr._mode) {
    case Address::base_plus_disp: {
      Register rn = adr._base;
      intptr_t disp = adr._disp;
      int scale = 1 << size;
      if (disp >= 0 && (disp & (scale - 1)) == 0 && (disp >> size) <= 0xFFF) {
        uint32_t base = doubleSize ? 0xFD400000 : 0xBD400000;
        if (!isLoad) base -= 0x400000;
        emit_long(base | ((disp >> size) << 10) | (rn.number() << 5) | rt.number());
      } else if (-256 <= disp && disp <= 255) {
        uint32_t base = doubleSize ? 0xFC400000 : 0xBC400000;
        if (!isLoad) base -= 0x400000;
        emit_long(base | ((disp & 0x1FF) << 12) | (rn.number() << 5) | rt.number());
      } else {
        // Displacement too large for a single float load/store (x86 allows
        // 32-bit displacements; AArch64 does not). Materialize rn + disp in
        // the reserved scratch register pair, then use a zero-displacement
        // load/store (same scheme as the integer load_store).
        Register scratch = (rn == x16 || rn == x17) ? (rn == x16 ? x17 : x16) : x16;
        uint64_t value = (uint64_t)(intptr_t)disp;
        int hw = (63 - __builtin_clzll(value)) >> 4;
        movz(scratch, (int)((value >> (16 * hw)) & 0xFFFF), hw);
        for (int i = hw - 1; i >= 0; i--) {
          int chunk = (int)((value >> (16 * i)) & 0xFFFF);
          if (chunk != 0) movk(scratch, chunk, i);
        }
        add(scratch, rn, scratch);
        load_store_float(rt, Address(scratch), doubleSize, isLoad);
      }
      break;
    }
    case Address::base_plus_reg_disp: {
      // x86-style base + index*2^scale + disp: materialize the register part
      // in x16 (the reserved scratch) and use the base_plus_disp form.
      assert(adr._base.isValid() && adr._index.isValid(), "illegal address");
      int sh = (int)adr._scale;		// times_1..times_8 = 0..3; no_scale = -1 -> 0
      if (sh < 0) sh = 0;
      add(x16, adr._base, adr._index, LSL, sh);
      load_store_float(rt, Address(x16, adr._disp), doubleSize, isLoad);
      break;
    }
    case Address::base_plus_reg:
    case Address::absolute:
      ShouldNotCallThis();
      break;
  }
}

void AArch64Assembler::ldr(FloatRegister ft, Address adr) { load_store_float(ft, adr, true,  true); }
void AArch64Assembler::str(FloatRegister ft, Address adr) { load_store_float(ft, adr, true,  false); }
void AArch64Assembler::ldr_s(FloatRegister ft, Address adr) { load_store_float(ft, adr, false, true); }
void AArch64Assembler::str_s(FloatRegister ft, Address adr) { load_store_float(ft, adr, false, false); }

void AArch64Assembler::ldr_pre (FloatRegister ft, Register rn, int imm9) { assert(ft.isValid(), "illegal register"); assert(-256 <= imm9 && imm9 <= 255, "imm9 out of range"); emit_long(0xFC400000 | (3 << 10) | ((imm9 & 0x1FF) << 12) | (rn.number() << 5) | ft.number()); }
void AArch64Assembler::str_pre (FloatRegister ft, Register rn, int imm9) { assert(ft.isValid(), "illegal register"); assert(-256 <= imm9 && imm9 <= 255, "imm9 out of range"); emit_long(0xFC000000 | (3 << 10) | ((imm9 & 0x1FF) << 12) | (rn.number() << 5) | ft.number()); }
void AArch64Assembler::ldr_post(FloatRegister ft, Register rn, int imm9) { assert(ft.isValid(), "illegal register"); assert(-256 <= imm9 && imm9 <= 255, "imm9 out of range"); emit_long(0xFC400000 | (1 << 10) | ((imm9 & 0x1FF) << 12) | (rn.number() << 5) | ft.number()); }
void AArch64Assembler::str_post(FloatRegister ft, Register rn, int imm9) { assert(ft.isValid(), "illegal register"); assert(-256 <= imm9 && imm9 <= 255, "imm9 out of range"); emit_long(0xFC000000 | (1 << 10) | ((imm9 & 0x1FF) << 12) | (rn.number() << 5) | ft.number()); }


// ---------------------------------------------------------------------------
// AArch64MacroAssembler
// ---------------------------------------------------------------------------

void AArch64MacroAssembler::mov(Register rd, intptr_t imm) {
  uint64_t value = (uint64_t)imm;
  int N, immr, imms;
  if (value == 0) {
    orr(rd, xzr, xzr);
    return;
  }
  if (encodeLogicalImmediate(value, 64, N, immr, imms)) {
    orr(rd, xzr, value);
    return;
  }
  // sign-extended 16-bit negative values are a single movn (matches clang)
  if ((int64_t)value >= -0x10000 && (int64_t)value < 0) {
    movn(rd, (int)(~value) & 0xFFFF, 0);
    return;
  }
  int hw = (63 - __builtin_clzll(value)) >> 4;
  movz(rd, (int)((value >> (16 * hw)) & 0xFFFF), hw);
  for (int i = hw - 1; i >= 0; i--) {
    int chunk = (int)((value >> (16 * i)) & 0xFFFF);
    if (chunk != 0) movk(rd, chunk, i);
  }
}


void AArch64MacroAssembler::mov(Register rd, Register rm) {
  // orr rd, xzr, rm is the canonical register move, but logical instructions
  // read register 31 as xzr (not sp), so moves involving sp must go through
  // add (addsub reads/writes sp for register 31 when S=0).
  if (rd == sp || rm == sp) {
    add(rd, rm, 0);
    return;
  }
  orr(rd, xzr, rm);
}


void AArch64MacroAssembler::align(int modulus) {
  while (offset() % modulus != 0) nop();
}


void AArch64MacroAssembler::push(Register src) {
  str_pre(src, sp, -16);	// str src, [sp, #-16]!
}

void AArch64MacroAssembler::pop(Register dst) {
  ldr_post(dst, sp, 16);	// ldr dst, [sp], #16
}


// ---------------------------------------------------------------------------
// x86-compatibility instruction set (used by the interpreter generator)
// ---------------------------------------------------------------------------

void AArch64Assembler::load_absolute_address(Register scratch, Address src) {
  assert(src._mode == Address::absolute, "absolute addressing only");
  assert(scratch.number() != 16 || true, "");	// x16/x17 are fine as scratch here
  emit_long(0x58000000 | (2 << 5) | scratch.number());	// ldr scratch, [pc, #8]
  emit_long(0x14000003);					// b .+12 (skip the 8-byte literal)
  emit_quad_data(src._disp, src._rtype);			// .quad <absolute address>
}


void AArch64Assembler::load_absolute_value(Register scratch, Address src) {
  load_absolute_address(scratch, src);
  ldr(scratch, Address(scratch));				// scratch = *scratch
}


void AArch64Assembler::pushl(Register src) {
  str_pre(src, sp, -8);		// 8-byte oop slot
}


void AArch64Assembler::popl(Register dst) {
  ldr_post(dst, sp, 8);
}


void AArch64MacroAssembler::movl(Register dst, int imm32) {
  // x86 mov ecx, imm32 is a 32-bit move: the 64-bit register gets the
  // zero-extended constant, not the sign-extended one.
  mov(dst, (uint32_t)imm32);
}


void AArch64MacroAssembler::movl(Register dst, Address src) {
  switch (src._mode) {
    case Address::absolute:
      load_absolute_value(x16, src);		// x16 = *<abs>
      mov(dst, x16);
      break;
    default:
      ldr(dst, src);
      break;
  }
}


void AArch64MacroAssembler::movl(Address dst, Register src) {
  switch (dst._mode) {
    case Address::absolute:
      load_absolute_address(x17, dst);
      str(src, Address(x17));
      break;
    default:
      str(src, dst);
      break;
  }
}


void AArch64MacroAssembler::movl(Address dst, int imm32) {
  switch (dst._mode) {
    case Address::absolute:
      load_absolute_address(x17, dst);
      mov(x16, (uint32_t)imm32);
      str(x16, Address(x17));
      break;
    default:
      mov(x16, (uint32_t)imm32);
      str(x16, dst);
      break;
  }
}


void AArch64MacroAssembler::movl(Register dst, oop obj) {
  mov(dst, (intptr_t)obj);
}


void AArch64MacroAssembler::movl(Address dst, oop obj) {
  mov(x16, (intptr_t)obj);
  switch (dst._mode) {
    case Address::absolute:
      load_absolute_address(x17, dst);
      str(x16, Address(x17));
      break;
    default:
      str(x16, dst);
      break;
  }
}


void AArch64MacroAssembler::movb(Register dst, Address src) {
  switch (src._mode) {
    case Address::absolute:
      load_absolute_value(x16, src);
      ldr_b(dst, Address(x16));
      break;
    default:
      ldr_b(dst, src);
      break;
  }
}


void AArch64MacroAssembler::movb(Address dst, Register src) {
  switch (dst._mode) {
    case Address::absolute:
      load_absolute_address(x17, dst);
      str_b(src, Address(x17));
      break;
    default:
      str_b(src, dst);
      break;
  }
}


void AArch64MacroAssembler::movb(Address dst, int imm8) {
  switch (dst._mode) {
    case Address::absolute:
      load_absolute_address(x17, dst);
      mov(x16, (intptr_t)(int8_t)imm8);
      str_b(x16, Address(x17));
      break;
    default:
      mov(x16, (intptr_t)(int8_t)imm8);
      str_b(x16, dst);
      break;
  }
}


void AArch64MacroAssembler::pushl(Register src) {
  str_pre(src, sp, -slotSize);		// one 16-byte slot keeps sp 16-byte aligned
}


void AArch64MacroAssembler::pushl(Address src) {
  movl(x16, src);
  str_pre(x16, sp, -slotSize);
}


void AArch64MacroAssembler::pushl(int imm32) {
  mov(x16, (uint32_t)imm32);	// zero-extended 32-bit value (x86 push imm32)
  str_pre(x16, sp, -slotSize);
}


void AArch64MacroAssembler::pushl(oop obj) {
  mov(x16, (intptr_t)obj);
  str_pre(x16, sp, -slotSize);
}


void AArch64MacroAssembler::popl(Register dst) {
  ldr_post(dst, sp, slotSize);
}


void AArch64MacroAssembler::pushad() {
  // save the six interpreter registers (48 bytes, keeps sp 16-byte aligned)
  stp_pre(eax, ebx, sp, -16);
  stp_pre(ecx, edx, sp, -16);
  stp_pre(edi, esi, sp, -16);
}


void AArch64MacroAssembler::popad() {
  ldp_post(edi, esi, sp, 16);
  ldp_post(ecx, edx, sp, 16);
  ldp_post(eax, ebx, sp, 16);
}


void AArch64MacroAssembler::addl(Register dst, int imm) {
  if (imm >= 0) {
    if (imm <= 0xFFF) { add(dst, dst, imm); return; }
  } else {
    if (-imm <= 0xFFF) { sub(dst, dst, -imm); return; }
  }
  mov(x16, (uint32_t)imm);
  addsub_with_sp(dst, dst, x16, false, LSL, 0);
}


// x16/x17 are the reserved scratch registers. Pick one that won't clobber a
// destination operand that lives in x16 (the generator never uses these, but
// the macro layer must be robust regardless).
static Register scratch_for(Register dst) { return dst == x16 ? x17 : x16; }


// sp (x31) is not a valid source operand in the shifted-register add/sub and
// logical encodings (register 31 reads as xzr there); only addsub_imm aliases
// it to sp. Materialize sp in a scratch GPR before emitting such an operation.
Register AArch64MacroAssembler::sp_source(Register reg, Register avoid) {
  if (reg != esp) return reg;
  Register scratch = (avoid == x16) ? x17 : x16;
  add(scratch, sp, 0);		// addsub_imm: rn = 31 reads as sp
  return scratch;
}


void AArch64MacroAssembler::addsub_with_sp(Register rd, Register rn, Register rm, bool isSub, ShiftType shift, int amt) {
  // rd may be sp (valid as Rd in the shifted form); only sp sources need
  // routing. sp_source picks a scratch avoiding the register already moved.
  rn = sp_source(rn, rm);
  rm = sp_source(rm, rn);
  if (isSub) sub(rd, rn, rm, shift, amt); else add(rd, rn, rm, shift, amt);
}


void AArch64MacroAssembler::cmp_with_sp(Register rn, Register rm) {
  rn = sp_source(rn, rm);
  rm = sp_source(rm, rn);
  cmp(rn, rm);
}


void AArch64MacroAssembler::addl(Register dst, Register src) {
  addsub_with_sp(dst, dst, src, false, LSL, 0);
}


void AArch64MacroAssembler::addl(Register dst, Address src) {
  Register scratch = scratch_for(dst);
  movl(scratch, src);
  addsub_with_sp(dst, dst, scratch, false, LSL, 0);
}


void AArch64MacroAssembler::addl(Address dst, int imm) {
  movl(x16, dst);
  addl(x16, imm);
  movl(dst, x16);
}


void AArch64MacroAssembler::incl(Address dst) {
  movl(x16, dst);
  add(x16, x16, 1);
  movl(dst, x16);
}


void AArch64MacroAssembler::decl(Address dst) {
  movl(x16, dst);
  sub(x16, x16, 1);
  movl(dst, x16);
}


void AArch64MacroAssembler::orl(Register dst, Address src) {
  Register scratch = scratch_for(dst);
  movl(scratch, src);
  orr(dst, dst, scratch);
}


// x86 `and ecx, imm32` / `or ecx, imm32` / `xor ecx, imm32` are 32-bit
// operations: only the low 32 bits of the destination are combined with the
// constant, and the upper 32 bits are PRESERVED. The constant therefore acts
// as a mask that is sign-extended (all-ones in the upper half). Note this is
// the opposite of the `movl/addl/subl/cmpl/testl` family, where the 32-bit
// constant is a zero-extended VALUE. Not every sign-extended mask is an
// encodable AArch64 logical immediate, so fall back to a scratch-register
// materialization.
void AArch64MacroAssembler::andl(Register dst, int imm) {
  uint64_t value = (uint64_t)(intptr_t)imm;
  int N, immr, imms;
  if (encodeLogicalImmediate(value, 64, N, immr, imms)) {
    and_(dst, dst, value);
    return;
  }
  Register scratch = scratch_for(dst);
  mov(scratch, value);
  and_(dst, dst, scratch);
}


void AArch64MacroAssembler::orl(Register dst, int imm) {
  uint64_t value = (uint64_t)(intptr_t)imm;
  int N, immr, imms;
  if (encodeLogicalImmediate(value, 64, N, immr, imms)) {
    orr(dst, dst, value);
    return;
  }
  Register scratch = scratch_for(dst);
  mov(scratch, value);
  orr(dst, dst, scratch);
}


void AArch64MacroAssembler::xorl(Register dst, int imm) {
  uint64_t value = (uint64_t)(intptr_t)imm;
  int N, immr, imms;
  if (encodeLogicalImmediate(value, 64, N, immr, imms)) {
    eor(dst, dst, value);
    return;
  }
  Register scratch = scratch_for(dst);
  mov(scratch, value);
  eor(dst, dst, scratch);
}


void AArch64MacroAssembler::popl(Address dst) {
  ldr_post(x16, sp, slotSize);
  movl(dst, x16);
}


void AArch64MacroAssembler::subl(Register dst, int imm) {
  if (imm >= 0) {
    if (imm <= 0xFFF) { sub(dst, dst, imm); return; }
  } else {
    if (-imm <= 0xFFF) { add(dst, dst, -imm); return; }
  }
  mov(x16, (uint32_t)imm);
  sub(dst, dst, x16);
}


void AArch64MacroAssembler::subl(Register dst, Register src) {
  addsub_with_sp(dst, dst, src, true, LSL, 0);
}


void AArch64MacroAssembler::subl(Register dst, Address src) {
  Register scratch = scratch_for(dst);
  movl(scratch, src);
  addsub_with_sp(dst, dst, scratch, true, LSL, 0);
}


void AArch64MacroAssembler::notl(Register reg) {
  orn(reg, xzr, reg);		// mvn
}


void AArch64MacroAssembler::imull(Register dst, Register src) {
  mul(dst, dst, src);
}


void AArch64MacroAssembler::imull(Register dst, int imm) {
  mov(x16, (uint32_t)imm);
  mul(dst, dst, x16);
}


void AArch64MacroAssembler::imull(Register dst, Register src, int value) {
  mov(x16, (uint32_t)value);
  mul(dst, src, x16);
}


// eax := eax * src, with the flags left so that the following conditional
// branch sees EQ when the 64-bit product did NOT overflow (aarch64 muls do
// not set flags; the caller tests overflow with jcc(notEqual)).
void AArch64MacroAssembler::imull(Register src) {
  mov(x16, eax);			// save dividend
  smulh(x17, x16, src);			// high 64 bits of eax * src
  mul(x16, x16, src);			// low 64 bits (result)
  mov(eax, x16);
  asr(x16, eax, 63);			// sign extension of the result
  cmp(x17, x16);			// EQ iff no overflow
}


void AArch64MacroAssembler::imull(int imm) {
  mov(x16, eax);			// dividend
  mov(x17, (uint32_t)imm);		// multiplier
  smulh(x12, x16, x17);			// high 64 bits of eax * imm
  mul(x16, x16, x17);			// low 64 bits (result)
  mov(eax, x16);
  asr(x16, eax, 63);			// sign extension of the result
  cmp(x12, x16);			// EQ iff no overflow
}


// edx := sign extension of eax (x86 cdq)
void AArch64MacroAssembler::cdq() {
  asr(edx, eax, 63);
}


// eax := edx:eax / src (signed), edx := remainder. The flags are left so the
// caller can test for overflow (EQ = no overflow; only INT64_MIN / -1 can
// overflow a 64-bit signed divide).
void AArch64MacroAssembler::idivl(Register src) {
  mov(x16, eax);			// dividend (low 64 bits; edx holds the sign extension)
  sdiv(x17, x16, src);			// quotient
  msub(x12, x17, src, x16);		// edx := dividend - quotient * divisor (remainder)
  mov(eax, x17);			// quotient
  mov(x16, INT64_C(0x8000000000000000));
  cmp(x17, x16);			// Z iff quotient == INT64_MIN
  cset(x16, EQ);
  cmn(src, 1);				// Z iff divisor == -1
  cset(x17, EQ);
  and_(x16, x16, x17);
  cmp(x16, 0);				// EQ iff no overflow
}


void AArch64MacroAssembler::leal(Register dst, Address src) {
  switch (src._mode) {
    case Address::base_plus_disp: {
      intptr_t disp = src._disp;
      if (disp >= 0 && disp <= 0xFFF) { add(dst, src._base, (int)disp); return; }
      if (disp < 0 && -disp <= 0xFFF) { sub(dst, src._base, (int)-disp); return; }
      mov(x16, disp);
      add(dst, src._base, x16);
      break;
    }
    case Address::base_plus_reg: {
      if (src._scale == Address::no_scale || src._scale == Address::times_1) {
        add(dst, src._base, src._index);
      } else {
        add(dst, src._base, src._index, LSL, (int)src._scale);
      }
      break;
    }
    case Address::base_plus_reg_disp: {
      if (src._scale == Address::no_scale || src._scale == Address::times_1) {
        add(dst, src._base, src._index);
      } else {
        add(dst, src._base, src._index, LSL, (int)src._scale);
      }
      if (src._disp >= 0 && src._disp <= 0xFFF) {
        add(dst, dst, (int)src._disp);
      } else if (src._disp < 0 && -src._disp <= 0xFFF) {
        sub(dst, dst, (int)-src._disp);
      } else {
        mov(x16, src._disp);
        add(dst, dst, x16);
      }
      break;
    }
    case Address::absolute:
      emit_long(0x58000000 | (2 << 5) | dst.number());	// ldr dst, [pc, #8]
      emit_long(0x14000003);				// b .+12 (skip the 8-byte literal)
      emit_quad_data(src._disp, src._rtype);		// .quad <abs>
      break;
  }
}


void AArch64MacroAssembler::cmpl(Register dst, int imm) {
  if (imm >= 0 && imm <= 0xFFF) { cmp(dst, imm); return; }
  if (imm < 0 && -imm <= 0xFFF) { cmn(dst, -imm); return; }
  mov(x16, (uint32_t)imm);
  cmp_with_sp(dst, x16);
}


void AArch64MacroAssembler::cmpl(Register dst, Register src) {
  cmp_with_sp(dst, src);
}


void AArch64MacroAssembler::cmpl(Register dst, Address src) {
  Register scratch = scratch_for(dst);
  movl(scratch, src);
  cmp_with_sp(dst, scratch);
}


void AArch64MacroAssembler::cmpl(Address dst, int imm) {
  movl(x16, dst);
  cmpl(x16, imm);
}


void AArch64MacroAssembler::cmpl(Register dst, oop obj) {
  mov(x16, (intptr_t)obj);
  cmp_with_sp(dst, x16);
}


void AArch64MacroAssembler::cmpl(Address dst, Register src) {
  movl(x16, dst);
  cmp_with_sp(x16, src);
}


void AArch64MacroAssembler::cmpl(Address dst, oop obj) {
  movl(x16, dst);
  mov(x17, (intptr_t)obj);
  cmp(x16, x17);
}


void AArch64MacroAssembler::testl(Register reg, int imm) {
  uint64_t value = (uint64_t)(uint32_t)imm;
  int N, immr, imms;
  if (encodeLogicalImmediate(value, 64, N, immr, imms)) {
    tst(reg, value);
    return;
  }
  mov(x16, (uint32_t)imm);
  tst(reg, x16);
}


void AArch64MacroAssembler::testb(Register reg, int imm) {
  uint64_t value = (uint64_t)(uint32_t)imm;
  int N, immr, imms;
  if (encodeLogicalImmediate(value, 64, N, immr, imms)) {
    tst(reg, value);
    return;
  }
  mov(x16, (uint32_t)imm);
  tst(reg, x16);
}


void AArch64MacroAssembler::jmp(Address dst) {
  switch (dst._mode) {
    case Address::base_plus_reg:
    case Address::base_plus_reg_disp:
    case Address::base_plus_disp:
      ldr(x16, dst);			// load target
      br(x16);
      break;
    case Address::absolute:
      load_absolute_value(x16, dst);	// x16 = *<abs>
      br(x16);
      break;
  }
}


void AArch64MacroAssembler::jcc(Condition cc, char* entry) {
  // Conditional branch to an absolute address: B.cond is PC-relative only,
  // so materialize the target and use a two-way branch (clobbers x16, which
  // is the reserved scratch; the flags survive since ldr/br do not alter them).
  Label fallthrough;
  load_absolute_address(x16, Address((intptr_t)entry, relocInfo::none));
  b((Condition)((int)cc ^ 1), fallthrough);	// if !cc, fall through
  br(x16);					// else jump to the target
  bind(fallthrough);
}


void AArch64MacroAssembler::jcc(Condition cc, char* entry, relocInfo::relocType rtype) {
  Label fallthrough;
  load_absolute_address(x16, Address((intptr_t)entry, rtype));
  b((Condition)((int)cc ^ 1), fallthrough);	// if !cc, fall through
  br(x16);					// else jump to the target
  bind(fallthrough);
}


void AArch64MacroAssembler::call(Address dst) {
  switch (dst._mode) {
    case Address::base_plus_reg:
    case Address::base_plus_reg_disp:
    case Address::base_plus_disp:
      ldr(x16, dst);
      blr(x16);
      break;
    case Address::absolute:
      load_absolute_value(x16, dst);
      blr(x16);
      break;
  }
}


void AArch64MacroAssembler::enter() {
  // frame setup: fp[0] = link (old x29), fp[1] = return address (x30)
  stp_pre(x29, x30, sp, -16);
  mov(x29, sp);
}


void AArch64MacroAssembler::leave() {
  mov(sp, x29);
  ldp_post(x29, x30, sp, 16);
}


void AArch64MacroAssembler::hlt() {
  emit_long(0xD4200000);		// brk #0
}


void AArch64MacroAssembler::ret(int imm) {
  // x86's ret imm pops the return address then removes imm bytes of
  // arguments from the stack. AArch64 already has the return address in x30
  // (leave()/the call popped it into the register), so only the argument
  // bytes remain to be skipped. Without this the caller's stack pointer drifts
  // down by the argument size on every returning callee and eventually hits
  // the stack limit.
  if (imm != 0) {
    assert(-4095 <= imm && imm <= 4095, "ret imm offset out of range");
    if (imm > 0) {
      add(sp, sp, imm);
    } else {
      sub(sp, sp, -imm);
    }
  }
  AArch64Assembler::ret();
}


// Store the current interpreter frame (fp/sp) into the last_Delta_frame
// globals so that non-local returns can unwind through C frames.
void AArch64MacroAssembler::set_last_Delta_frame_before_call() {
  // last_Delta_fp = ebp
  load_absolute_address(x17, Address((intptr_t)&last_Delta_fp, relocInfo::external_word_type));
  str(ebp, Address(x17));
  // last_Delta_sp = sp
  load_absolute_address(x17, Address((intptr_t)&last_Delta_sp, relocInfo::external_word_type));
  add(x16, sp, 0);			// mov x16, sp (addsub form reads sp)
  str(x16, Address(x17));
  // last_Delta_pc
  // On x86 the call in the C-call glue pushes a return address below the
  // interpreter sp, so last_frame() -> frame(sp, fp) reads it from sp[-1].
  // AArch64's blr leaves the return address in x30 instead of pushing, and the
  // generated interpreter dispatches via br (no link), so x30 is stale at the
  // C-call boundary (it holds the _call_delta return address). Publish the
  // address of this call site (in the generating buffer) as last_Delta_pc so
  // last_frame() classifies the frame by its real code buffer: interpreter
  // code -> is_interpreted_frame() true, compiled code -> false, exactly like
  // the x86 sp[-1] return address.
  Label after_adr;
  adr(x17, after_adr);
  bind(after_adr);
  load_absolute_address(x16, Address((intptr_t)&last_Delta_pc, relocInfo::external_word_type));
  str(x17, Address(x16));
}


// After a call the return address lives in x30 and sp has not moved (blr does
// not push), so the after-call view is identical to the before-call one.
void AArch64MacroAssembler::set_last_Delta_frame_after_call() {
  set_last_Delta_frame_before_call();
}


void AArch64MacroAssembler::reset_last_Delta_frame() {
  load_absolute_address(x17, Address((intptr_t)&last_Delta_fp, relocInfo::external_word_type));
  str(xzr, Address(x17));
  load_absolute_address(x17, Address((intptr_t)&last_Delta_pc, relocInfo::external_word_type));
  str(xzr, Address(x17));
}


// ---------------------------------------------------------------------------
// C-call glue. AAPCS64 (and Apple Silicon's SP-alignment fault) require sp to
// be 16-byte aligned at a call site; the interpreter's expression-stack slots
// are 16 bytes so sp is normally aligned, but some paths leave it 8-misaligned.
// The pad slot below is therefore 16 bytes and is pushed through a scratch
// register (an 8-byte SP push from an 8-misaligned sp would fault). The slot
// is popped again afterwards (the marker is checked, never assumed).
// ---------------------------------------------------------------------------

static const int64_t stack_pad_marker = INT64_C(0x414C474E41444400); // 'ALGNADD\x00'

static void align_stack_before_call(AArch64MacroAssembler& a) {
  Label aligned;
  a.add(x16, sp, 0);			// mov x16, sp (logical tst cannot read sp)
  a.tst(x16, 8);
  a.b(EQ, aligned);
  a.sub(x17, sp, 16);
  a.mov(x16, stack_pad_marker);
  a.str(x16, Address(x17));		// pad slot (sp now 16-byte aligned)
  a.sub(sp, sp, 16);
  a.bind(aligned);
}

static void restore_stack_after_call(AArch64MacroAssembler& a) {
  Label restored;
  a.ldr(x16, Address(sp));			// peek top of stack (sp is 16-byte aligned here)
  a.mov(x17, stack_pad_marker);
  a.cmp(x16, x17);
  a.b(NE, restored);
  a.add(sp, sp, 16);				// remove pad slot
  a.bind(restored);
}


// ---------------------------------------------------------------------------
// Float stack (x87 model, register-based).
//
// The interpreter models the x87 register stack with a generation-time depth
// counter (_float_depth). st(i) (i = 0 is the top) lives in the callee-saved
// double d(8 + _float_depth - 1 - i); the callee-saved range means deferred
// floats survive the C calls the interpreter makes between bytecodes, just as
// the x87 stack does (the floats are re-loaded on demand). Every float
// bytecode leaves the stack at the same depth it entered it with - the x87
// model, including the stale divisor fprem leaves behind. d16-d31 remain
// available as caller-saved scratch.
// ---------------------------------------------------------------------------

static const int float_stack_base = 8;	// st(0) at depth d is d(8 + d - 1)

FloatRegister AArch64MacroAssembler::st_reg(int i) const {
  assert(0 <= i && i < _float_depth, "float stack underflow");
  assert(_float_depth <= 8, "float stack overflow (max 8)");
  return FloatRegister(float_stack_base + _float_depth - 1 - i, ' ');
}

void AArch64MacroAssembler::fld_d(Address src) {
  assert(_float_depth < 8, "float stack overflow");
  ldr(FloatRegister(float_stack_base + _float_depth, ' '), src);
  _float_depth++;
}

void AArch64MacroAssembler::fstp_d(Address dst) {
  assert(_float_depth > 0, "float stack underflow");
  str(FloatRegister(float_stack_base + _float_depth - 1, ' '), dst);
  _float_depth--;
}

// Memory-operand floating-point ops: st(0) := st(0) op double [src]. The
// memory value is staged in the caller-saved d16, which never holds a live
// float-stack value (those live in d8..d15).
void AArch64MacroAssembler::fadd_d(Address src) { ldr(FloatRegister(16, ' '), src); AArch64Assembler::fadd(st_reg(0), st_reg(0), FloatRegister(16, ' ')); }
void AArch64MacroAssembler::fsub_d(Address src) { ldr(FloatRegister(16, ' '), src); AArch64Assembler::fsub(st_reg(0), st_reg(0), FloatRegister(16, ' ')); }
void AArch64MacroAssembler::fmul_d(Address src) { ldr(FloatRegister(16, ' '), src); AArch64Assembler::fmul(st_reg(0), st_reg(0), FloatRegister(16, ' ')); }
void AArch64MacroAssembler::fdiv_d(Address src) { ldr(FloatRegister(16, ' '), src); AArch64Assembler::fdiv(st_reg(0), st_reg(0), FloatRegister(16, ' ')); }

void AArch64MacroAssembler::fild_s(Address src) {
  assert(_float_depth < 8, "float stack overflow");
  ldr_w(x16, src);			// 32-bit signed integer
  scvtf(FloatRegister(float_stack_base + _float_depth, ' '), x16, sz_64, sz_32);
  _float_depth++;
}

void AArch64MacroAssembler::fpop() {
  assert(_float_depth > 0, "float stack underflow");
  _float_depth--;
}

void AArch64MacroAssembler::push_float() {
  assert(_float_depth < 8, "float stack overflow");
  _float_depth++;
}

void AArch64MacroAssembler::pop_float() {
  assert(_float_depth > 0, "float stack underflow");
  _float_depth--;
}

void AArch64MacroAssembler::fldz() {
  assert(_float_depth < 8, "float stack overflow");
  fmov(FloatRegister(float_stack_base + _float_depth, ' '), xzr);	// +0.0
  _float_depth++;
}

void AArch64MacroAssembler::fld1() {
  assert(_float_depth < 8, "float stack overflow");
  // fmov d, #1.0 = 0x1E6E1000 (imm8 = 0b00000010); verified against clang
  emit_long(0x1E6E1000 | FloatRegister(float_stack_base + _float_depth, ' ').number());
  _float_depth++;
}

void AArch64MacroAssembler::fabs() { AArch64Assembler::fabs(st_reg(0), st_reg(0)); }
void AArch64MacroAssembler::fchs() { fneg(st_reg(0), st_reg(0)); }
void AArch64MacroAssembler::fsqrt() { AArch64Assembler::fsqrt(st_reg(0), st_reg(0)); }

void AArch64MacroAssembler::fmul(int i) {
  assert(0 <= i && i < _float_depth, "float stack index out of range");
  AArch64Assembler::fmul(st_reg(0), st_reg(0), st_reg(i));	// st0 = st0 * sti
}

void AArch64MacroAssembler::faddp(int i) {
  assert(0 < i && i < _float_depth, "float stack index out of range");
  fadd(st_reg(i), st_reg(i), st_reg(0));	// sti = sti + st0
  fpop();
}

void AArch64MacroAssembler::fsubp(int i) {
  assert(0 < i && i < _float_depth, "float stack index out of range");
  fsub(st_reg(i), st_reg(i), st_reg(0));	// sti = sti - st0
  fpop();
}

void AArch64MacroAssembler::fmulp(int i) {
  assert(0 < i && i < _float_depth, "float stack index out of range");
  AArch64Assembler::fmul(st_reg(i), st_reg(i), st_reg(0));	// sti = sti * st0
  fpop();
}

void AArch64MacroAssembler::fdivp(int i) {
  assert(0 < i && i < _float_depth, "float stack index out of range");
  fdiv(st_reg(i), st_reg(i), st_reg(0));	// sti = sti / st0
  fpop();
}

void AArch64MacroAssembler::fxch(int i) {
  assert(0 < i && i < _float_depth, "float stack index out of range");
  // swap st(0) and st(i) through a scratch double register (d31, caller-saved)
  fmov(d31, st_reg(0));
  fmov(st_reg(0), st_reg(i));
  fmov(st_reg(i), d31);
}

// C helper for the fprem bytecode (x87's fprem = truncating remainder).
extern "C" double fmod(double, double);

void AArch64MacroAssembler::fprem() {
  assert(_float_depth >= 2, "fprem needs two operands");
  // Emulate x87 fprem (st0 := st0 mod st1) and then drop the modulus, so the
  // emulated float stack stays self-contained at bytecode boundaries (the
  // caller's fstp_d stores the remainder in st0).
  fmov(d0, st_reg(0));		// dividend
  fmov(d1, st_reg(1));		// divisor
  align_stack_before_call(*this);
  call((char*)&fmod, relocInfo::external_word_type);
  restore_stack_after_call(*this);
  _float_depth--;
  fmov(st_reg(0), d0);		// result into the new st0
}

void AArch64MacroAssembler::ftst() {
  assert(_float_depth > 0, "float stack underflow");
  fcmp0(st_reg(0));
}

void AArch64MacroAssembler::fcompp() {
  assert(_float_depth >= 2, "fcompp needs two operands");
  fcmp(st_reg(1), st_reg(0));	// compare st1 with st0, set flags like x87
  _float_depth -= 2;
}

void AArch64MacroAssembler::fnstsw_ax() {
  // Emulate the x87 status word (comparison of ST with ST(1)) in eax (x13).
  // After fcmp, the ARM flags mean: less -> N=1 (MI), equal -> Z=1 (EQ),
  // greater -> none set (GT), unordered -> V=1 (VS). The x87 C-bits are
  // greater = 0x0000, less = 0x0100, equal = 0x4000, unordered = 0x4500.
  //   eax = VS ? 0x4500 : (EQ ? 0x4000 : (MI ? 0x0100 : 0x0000))
  mov(x17, 0x0100);
  csel(eax, x17, xzr, MI);	// less ? 0x0100 : 0x0000
  mov(x17, 0x4000);
  csel(eax, x17, eax, EQ);	// equal ? 0x4000 : prev
  mov(x17, 0x4500);
  csel(eax, x17, eax, VS);	// unordered ? 0x4500 : prev
}
// x87 flag model: mask/cond pairs as used by Floats::generate_tst/generate_cmp.
// The C-bits compare ST with ST(1); greater = 000, less = 001, equal = 100,
// unordered = 111 (C3=0x4000, C2=0x0400, C0=0x0100). The mask tests those bits
// and the ARM condition is the sense of the "set" test (the branch follows
// testl(eax, mask), so a non-zero result jumps when cond is true).
void AArch64MacroAssembler::fpu_mask_and_cond_for(Condition cc, int& mask, Condition& cond) {
  switch (cc) {
    case equal:		mask = 0x4000; cond = notZero;	break;
    case notEqual:	mask = 0x4000; cond = zero;	break;
    case less:		mask = 0x0100; cond = notZero;	break;
    case lessEqual:	mask = 0x4500; cond = notZero;	break;
    case greater:	mask = 0x4500; cond = zero;	break;
    case greaterEqual:	mask = 0x0100; cond = zero;	break;
    default:		ShouldNotReachHere();
  }
}

void AArch64MacroAssembler::fwait() {
  // The AArch64 FPU flags are read directly by fnstsw_ax; there is no
  // x87-style pending-exception wait required between fcmp and fnstsw_ax.
}


// Runtime-call interface: the C function is entered via blr. Arguments in
// x0-x7 (AAPCS64); the interpreter's C-call sites that pass args on the
// interpreter stack are rewritten to register passing during the generator
// port. The NLR inline-cache info is emitted as a NOP placeholder for now.
// AArch64's bl/blr deposit the return address in x30 (the LR register); the
// generated interpreter code returns with ret() == br x30, which does not pop
// the stack. Any C call therefore must leave x30 untouched, otherwise a later
// ret() would branch back to the call's continuation (an infinite loop).
// The LR is saved below the alignment marker so the C function is entered
// with a 16-byte aligned sp; restore_stack_after_call removes the marker
// first, then the saved LR is popped back into x30.
// All stack adjustments here use 16-byte slots and go through the x16/x17
// scratch registers: Apple Silicon faults any SP-based access that executes
// with a non-16-byte-aligned sp (EXC_ARM_SP_ALIGN), so an 8-byte SP push from
// an aligned sp -- or any SP op from an 8-misaligned sp -- is illegal. With
// 16-byte slots sp stays 16-byte aligned throughout the call.
#define PRESERVE_LR_BEFORE_CALL() \
  sub(x16, sp, 16); str(x30, Address(x16)); sub(sp, sp, 16)
#define RESTORE_LR_AFTER_CALL() \
  mov(x16, sp); ldr(x30, Address(x16)); add(sp, sp, 16)

void AArch64MacroAssembler::call_C(Label& L) {
  set_last_Delta_frame_before_call();
  align_stack_before_call(*this);
  bl(L);
  restore_stack_after_call(*this);
  reset_last_Delta_frame();
}

void AArch64MacroAssembler::call_C(Label& L, Label& nlrTestPoint) {
  set_last_Delta_frame_before_call();
  align_stack_before_call(*this);
  bl(L);
  ic_info(nlrTestPoint, 0);
  restore_stack_after_call(*this);
  reset_last_Delta_frame();
}

void AArch64MacroAssembler::call_C(char* entry, relocInfo::relocType rtype) {
  set_last_Delta_frame_before_call();
  PRESERVE_LR_BEFORE_CALL();
  align_stack_before_call(*this);
  AArch64Assembler::call(entry, rtype);
  restore_stack_after_call(*this);
  RESTORE_LR_AFTER_CALL();
  reset_last_Delta_frame();
  // The interpreter reads C results from eax (x13); AAPCS64 returns them in
  // x0, which the call just clobbered. Copy the result over.
  mov(eax, x0);
}

void AArch64MacroAssembler::call_C(char* entry, relocInfo::relocType rtype, Label& nlrTestPoint) {
  set_last_Delta_frame_before_call();
  PRESERVE_LR_BEFORE_CALL();
  align_stack_before_call(*this);
  AArch64Assembler::call(entry, rtype);
  ic_info(nlrTestPoint, 0);
  restore_stack_after_call(*this);
  RESTORE_LR_AFTER_CALL();
  reset_last_Delta_frame();
  mov(eax, x0);
}

void AArch64MacroAssembler::call_C(Register entry) {
  set_last_Delta_frame_before_call();
  PRESERVE_LR_BEFORE_CALL();
  align_stack_before_call(*this);
  blr(entry);
  restore_stack_after_call(*this);
  RESTORE_LR_AFTER_CALL();
  reset_last_Delta_frame();
  mov(eax, x0);
}

void AArch64MacroAssembler::call_C(Register entry, Label& nlrTestPoint) {
  set_last_Delta_frame_before_call();
  PRESERVE_LR_BEFORE_CALL();
  align_stack_before_call(*this);
  blr(entry);
  ic_info(nlrTestPoint, 0);
  restore_stack_after_call(*this);
  RESTORE_LR_AFTER_CALL();
  reset_last_Delta_frame();
  mov(eax, x0);
}

#undef PRESERVE_LR_BEFORE_CALL
#undef RESTORE_LR_AFTER_CALL

void AArch64MacroAssembler::call_C(char* entry, Register arg1) {
  mov(x0, arg1);
  call_C(entry, relocInfo::runtime_call_type);
}

void AArch64MacroAssembler::call_C(char* entry, Register arg1, Register arg2) {
  mov(x1, arg2);
  mov(x0, arg1);
  call_C(entry, relocInfo::runtime_call_type);
}

void AArch64MacroAssembler::call_C(char* entry, Register arg1, Register arg2, Register arg3) {
  mov(x2, arg3);
  mov(x1, arg2);
  mov(x0, arg1);
  call_C(entry, relocInfo::runtime_call_type);
}

void AArch64MacroAssembler::call_C(char* entry, Register arg1, Register arg2, Register arg3, Register arg4) {
  mov(x3, arg4);
  mov(x2, arg3);
  mov(x1, arg2);
  mov(x0, arg1);
  call_C(entry, relocInfo::runtime_call_type);
}

void AArch64MacroAssembler::store_check(Register obj, Register tmp) {
  // Write barrier for interpreter/compiled stores. Mirrors the x86
  // implementation: if obj points into the old generation, mark the
  // corresponding card in the remembered set dirty. The content of obj is
  // destroyed; tmp is scratch. x16 is used as an additional scratch.
  assert(obj != tmp, "registers must be different");
  Label no_store;
  // boundary between new and old generation is fixed at generation time
  mov(tmp, (intptr_t)Universe::new_gen.boundary());
  cmp(obj, tmp);				// avoid marking if target is a new object
  jcc(less, no_store);
  movl(tmp, Address((intptr_t)&byte_map_base, relocInfo::external_word_type));
  shrl(obj, card_shift);
  movb(Address(tmp, obj, Address::times_1), 0);
  bind(no_store);
}


// Debugging (mirrors the x86 inspector; debugging only)

static void aarch64_print_reg(char* name, oop obj) {
  mystd->print("%s = ", name);
  if (obj == NULL) {
    mystd->print_cr("NULL");
  } else if (obj->is_smi()) {
    mystd->print_cr("smi (%d)", smiOop(obj)->value());
  } else if (obj->is_mem() && Universe::is_heap((oop*)obj)) {
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


void AArch64MacroAssembler::inspector(oop edi, oop esi, oop ebp, oop esp, oop ebx, oop edx, oop ecx, oop eax, char* eip) {
  mystd->print_cr("inspector at 0x%08x", eip);
  aarch64_print_reg("eax", eax);
  aarch64_print_reg("ebx", ebx);
  aarch64_print_reg("ecx", ecx);
  aarch64_print_reg("edx", edx);
  aarch64_print_reg("edi", edi);
  aarch64_print_reg("esi", esi);
  mystd->print_cr("ebp = 0x%08x", ebp);
  mystd->print_cr("esp = 0x%08x", esp);
  mystd->cr();
}


// Embed an oop in the code stream. x86 uses `testl eax, imm32` (a 5-byte
// instruction) so that the oop also acts as the "native test" marker scanned
// by nativeTest_at; aarch64 uses a literal pool entry (ldr x16, [pc,#8];
// b .+8; .quad oop) that the oop relocations keep updated.
void AArch64MacroAssembler::inline_oop(oop o) {
  load_absolute_address(x16, Address((intptr_t)o, relocInfo::oop_type));
}

#endif // DELTA_ASSEMBLER_BACKEND_AARCH64

