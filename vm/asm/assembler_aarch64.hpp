/*
Copyright (c) 2026, Gerardo Santana Gomez Garrido.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// The AArch64 assembler backend. It is selected by "asm/assembler.hpp"
// (via the Assembler/MacroAssembler aliases) when
// DELTA_ASSEMBLER_BACKEND_AARCH64 is defined; the default backend remains
// x86. It extends AbstractAssembler, implements the AArch64 (ARMv8-A) 64-bit
// instruction set and the backend-specific label fixup.
//
// All instructions are emitted as single 32-bit little-endian words. Every
// encoding in this file has been cross-verified byte-for-byte against the
// output of Apple clang (arm64-apple-macos11) for the corresponding
// instruction, see the test in test/assembler/assemblerEncoderTest_aarch64.cpp.
// The code has NOT been executed on real hardware yet (no native aarch64 host
// is available); labels/fixup and the exact FP semantics still need hardware
// validation during the JIT retarget phase.
//
// The general-purpose registers are x0..x30 (Register 31 is used as sp or
// xzr depending on the instruction context), the floating-point/vector
// registers are d0..d31 (used here in their scalar double/single precision
// forms). Branch targets are PC-relative; there is no adrp on Apple platforms
// (mach-O rejects local-label adrp), so label addressing goes through adr /
// ldr-literal.

#ifndef _ASSEMBLER_AARCH64_HPP
#define _ASSEMBLER_AARCH64_HPP

#include "asm/abstractAssembler.hpp"
#include "code/relocInfo.hpp"
#include "memory/allocation.hpp"

#include <cstdint>

const int BytesPerNativeWord = 8; // size of a native word (pointer) in bytes
const int nofRegisters = 32; // total number of general-purpose registers

// Register size for instructions that have both a 32-bit (W) and a 64-bit
// (X) form. The public instruction emitters default to the 64-bit form.
enum RegisterSize {
  sz_32 = 0,
  sz_64 = 1,
};

// Shift/extension types used by the shifted-register and add/sub shifted
// instructions.
enum ShiftType {
  LSL = 0,
  LSR = 1,
  ASR = 2,
  ROR = 3,
};

class Register : public ValueObj {
private:
  int _number;

public:
  // creation
  Register(void) : _number(-1) {}
  Register(int number, char f) : _number(number) {} // f is only to make sure that
  // an int is not accidentially
  // converted into a Register...

  // attributes
  int number() const {
    assert(isValid(), "not a register");
    return _number;
  }
  bool isValid() const { return (0 <= _number) && (_number < nofRegisters); }
  bool hasByteRegister() const { return 0 <= _number && _number < 31; }

  // comparison
  friend bool operator==(Register x, Register y) { return x._number == y._number; }
  friend bool operator!=(Register x, Register y) { return x._number != y._number; }

  // debugging
  char* name() const;
};

// Available general-purpose registers.
const Register x0 = Register(0, ' ');
const Register x1 = Register(1, ' ');
const Register x2 = Register(2, ' ');
const Register x3 = Register(3, ' ');
const Register x4 = Register(4, ' ');
const Register x5 = Register(5, ' ');
const Register x6 = Register(6, ' ');
const Register x7 = Register(7, ' ');
const Register x8 = Register(8, ' ');
const Register x9 = Register(9, ' ');
const Register x10 = Register(10, ' ');
const Register x11 = Register(11, ' ');
const Register x12 = Register(12, ' ');
const Register x13 = Register(13, ' ');
const Register x14 = Register(14, ' ');
const Register x15 = Register(15, ' ');
const Register x16 = Register(16, ' '); // IP0 - intra-procedure-call scratch register
const Register x17 = Register(17, ' '); // IP1
const Register x18 = Register(18, ' ');
const Register x19 = Register(19, ' ');
const Register x20 = Register(20, ' ');
const Register x21 = Register(21, ' ');
const Register x22 = Register(22, ' ');
const Register x23 = Register(23, ' ');
const Register x24 = Register(24, ' ');
const Register x25 = Register(25, ' ');
const Register x26 = Register(26, ' ');
const Register x27 = Register(27, ' ');
const Register x28 = Register(28, ' ');
const Register x29 = Register(29, ' '); // fp - frame pointer
const Register x30 = Register(30, ' '); // lr - link register

// Register 31 is the zero register or the stack pointer, depending on the
// instruction context (Rn of memory operations, Rm of add/sub, ... => sp).
const Register xzr = Register(31, ' ');
const Register sp = Register(31, ' ');

// Aliases used by the rest of the VM (see also asm/mapping_aarch64.hpp).
const Register fp = x29; // frame pointer
const Register lr = x30; // link register

// 32-bit aliases (same register numbers as x0..x30; the width of an operation
// is selected by the emitter's RegisterSize argument or by 32-bit-specific
// method names, not by the register object itself).
const Register w0 = Register(0, ' ');
const Register w1 = Register(1, ' ');
const Register w2 = Register(2, ' ');
const Register w3 = Register(3, ' ');
const Register w4 = Register(4, ' ');
const Register w5 = Register(5, ' ');
const Register w6 = Register(6, ' ');
const Register w7 = Register(7, ' ');
const Register w8 = Register(8, ' ');
const Register w9 = Register(9, ' ');
const Register w10 = Register(10, ' ');
const Register w11 = Register(11, ' ');
const Register w12 = Register(12, ' ');
const Register w13 = Register(13, ' ');
const Register w14 = Register(14, ' ');
const Register w15 = Register(15, ' ');
const Register w16 = Register(16, ' ');
const Register w17 = Register(17, ' ');
const Register w18 = Register(18, ' ');
const Register w19 = Register(19, ' ');
const Register w20 = Register(20, ' ');
const Register w21 = Register(21, ' ');
const Register w22 = Register(22, ' ');
const Register w23 = Register(23, ' ');
const Register w24 = Register(24, ' ');
const Register w25 = Register(25, ' ');
const Register w26 = Register(26, ' ');
const Register w27 = Register(27, ' ');
const Register w28 = Register(28, ' ');
const Register w29 = Register(29, ' ');
const Register w30 = Register(30, ' ');
const Register wzr = Register(31, ' ');

const Register noreg; // Dummy register used in Load, LoadAddr, and Store.

// Interpreter register allocation (see vm/interpreter/interpreter.cpp).
// The generator is written against the x86 register names; on AArch64 they
// map to dedicated general-purpose registers disjoint from the scratch
// registers x16/x17 (used internally to materialize absolute addresses) and
// from the mapping's temps x9/x10/x11 (used by compiled code, not by the
// interpreter).
//
//   eax  tos accumulator   -> x13
//   ebx  bytecode value    -> x15
//   ecx  scratch           -> x11
//   edx  scratch           -> x12
//   edi  dispatch target   -> x27
//   esi  bytecode pointer  -> x14
//   ebp  frame pointer     -> x29 (fp)
//   esp  stack pointer     -> sp
const Register eax = Register(13, ' ');
const Register ebx = Register(15, ' ');
const Register ecx = Register(11, ' ');
const Register edx = Register(12, ' ');
const Register edi = Register(27, ' ');
const Register esi = Register(14, ' ');
const Register ebp = Register(29, ' ');
const Register esp = sp;

// Floating-point/vector registers (used in scalar d/s forms).
class FloatRegister : public ValueObj {
private:
  int _number;

public:
  FloatRegister(void) : _number(-1) {}
  FloatRegister(int number, char f) : _number(number) {}

  int number() const {
    assert(isValid(), "not a register");
    return _number;
  }
  bool isValid() const { return (0 <= _number) && (_number < 32); }

  friend bool operator==(FloatRegister x, FloatRegister y) { return x._number == y._number; }
  friend bool operator!=(FloatRegister x, FloatRegister y) { return x._number != y._number; }

  char* name() const;
};

const FloatRegister d0 = FloatRegister(0, ' ');
const FloatRegister d1 = FloatRegister(1, ' ');
const FloatRegister d2 = FloatRegister(2, ' ');
const FloatRegister d3 = FloatRegister(3, ' ');
const FloatRegister d4 = FloatRegister(4, ' ');
const FloatRegister d5 = FloatRegister(5, ' ');
const FloatRegister d6 = FloatRegister(6, ' ');
const FloatRegister d7 = FloatRegister(7, ' ');
const FloatRegister d8 = FloatRegister(8, ' ');
const FloatRegister d9 = FloatRegister(9, ' ');
const FloatRegister d10 = FloatRegister(10, ' ');
const FloatRegister d11 = FloatRegister(11, ' ');
const FloatRegister d12 = FloatRegister(12, ' ');
const FloatRegister d13 = FloatRegister(13, ' ');
const FloatRegister d14 = FloatRegister(14, ' ');
const FloatRegister d15 = FloatRegister(15, ' ');
const FloatRegister d16 = FloatRegister(16, ' ');
const FloatRegister d17 = FloatRegister(17, ' ');
const FloatRegister d18 = FloatRegister(18, ' ');
const FloatRegister d19 = FloatRegister(19, ' ');
const FloatRegister d20 = FloatRegister(20, ' ');
const FloatRegister d21 = FloatRegister(21, ' ');
const FloatRegister d22 = FloatRegister(22, ' ');
const FloatRegister d23 = FloatRegister(23, ' ');
const FloatRegister d24 = FloatRegister(24, ' ');
const FloatRegister d25 = FloatRegister(25, ' ');
const FloatRegister d26 = FloatRegister(26, ' ');
const FloatRegister d27 = FloatRegister(27, ' ');
const FloatRegister d28 = FloatRegister(28, ' ');
const FloatRegister d29 = FloatRegister(29, ' ');
const FloatRegister d30 = FloatRegister(30, ' ');
const FloatRegister d31 = FloatRegister(31, ' ');

// AArch64 condition codes (NZCV). The x86-style aliases used by the compiler
// (zero/notZero, below/above, negative, ...) map onto the ARM conditions for
// the corresponding signed/unsigned relations.
class AArch64Assembler;

enum Condition {
  EQ = 0, // equal (Z)
  NE = 1, // not equal (!Z)
  CS = 2, // carry set (unsigned >=)
  HS = 2,
  CC = 3, // carry clear (unsigned <)
  LO = 3,
  MI = 4, // negative (N)
  PL = 5, // positive/zero (!N)
  VS = 6, // overflow (V)
  VC = 7, // no overflow (!V)
  HI = 8, // unsigned >  (!C & !Z)
  LS = 9, // unsigned <= (C & Z)
  GE = 10, // signed >=   (N == V)
  LT = 11, // signed <    (N != V)
  GT = 12, // signed >    (!Z & (N == V))
  LE = 13, // signed <=   (Z | (N != V))
  AL = 14, // always

  // x86-style aliases. NOTE: the operand order is the same as x86
  // (cmp dst, src computes dst - src), so unsigned conditions map as
  // below(=CF)/belowEqual/above/aboveEqual -> CC/LO, LS, HI, CS/HS.
  zero = EQ,
  notZero = NE,
  equal = EQ,
  notEqual = NE,
  less = LT,
  lessEqual = LE,
  greater = GT,
  greaterEqual = GE,
  below = CC, // unsigned <  (CF set)
  belowEqual = LS, // unsigned <=
  above = HI, // unsigned >
  aboveEqual = CS, // unsigned >= (CF clear)
  overflow = VS,
  noOverflow = VC,
  carrySet = CS,
  carryClear = CC,
  negative = MI,
  positive = PL,
};

// Address operands for assembler
//
// AArch64 has no x86-style scaled index addressing; the modes map as:
//   base_plus_disp  -> base + unsigned imm12 / unscaled imm9 (chosen by range)
//   base_plus_reg   -> base + index register (optionally scaled by 2^scale)
//   absolute        -> not encodable in a single instruction (see the notes
//                      in assembler_aarch64.cpp); reserved for future use

class Address : public ValueObj {
public:
  enum Mode {
    base_plus_disp,
    base_plus_reg,
    base_plus_reg_disp, // base + index*2^scale + disp (x86-style; materialized via scratch)
    absolute,
  };

  enum ScaleFactor {
    no_scale = -1, // unscaled register offset
    times_1 = 0, // scaled by 1 (byte)
    times_2 = 1, // scaled by 2 (halfword)
    times_4 = 2, // scaled by 4 (word)
    times_8 = 3, // scaled by 8 (doubleword)
    times_16 = 4 // scaled by 16 (delta stack slot; materialized via scratch)
  };

private:
  Mode _mode;
  Register _base;
  Register _index;
  ScaleFactor _scale;
  intptr_t _disp;
  relocInfo::relocType _rtype;

public:
  Address();
  Address(intptr_t disp, relocInfo::relocType rtype); // absolute
  Address(Register base, intptr_t disp = 0, relocInfo::relocType rtype = relocInfo::none); // base_plus_disp
  Address(Register base, Register index, ScaleFactor scale = no_scale,
          relocInfo::relocType rtype = relocInfo::none); // base_plus_reg
  // x86-style base + index*2^scale + disp; with noreg base/index this is
  // an absolute address, otherwise base_plus_reg_disp (or base_plus_disp
  // when the index is invalid).
  Address(Register base, Register index, ScaleFactor scale, intptr_t disp,
          relocInfo::relocType rtype = relocInfo::none);

  friend class AArch64Assembler;
  friend class AArch64MacroAssembler;
};

// The AArch64 assembler. It extends the common AbstractAssembler with the
// AArch64 instruction set and the AArch64 specific label fixup machinery.

class AArch64Assembler : public AbstractAssembler {
protected:
  // AArch64 label fixup
  void print(Label& L);
  void bind_to(Label& L, int pos);
  void link_to(Label& L, Label& appendix);

  // shared low-level encoders
  void logical_shifted(Register rd, Register rn, Register rm, int opc, bool N, ShiftType shift, int amt,
                       RegisterSize size = sz_64);
  void addsub_shifted(Register rd, Register rn, Register rm, bool op, bool S, ShiftType shift, int amt,
                      RegisterSize size = sz_64);
  void addsub_imm(Register rd, Register rn, int imm12, bool op, bool S, int sh, RegisterSize size = sz_64);
  void logical_imm(Register rd, Register rn, uint64_t imm, int opc, RegisterSize size = sz_64);
  void bitfield_op(Register rd, Register rn, int opc, int immr, int imms, RegisterSize size = sz_64);
  void load_store(Register rt, Address adr, int size, bool isLoad);
  void load_store_float(FloatRegister rt, Address adr, bool doubleSize, bool isLoad);

  // rd = base + index*2^scale, with a stack-pointer base handled correctly.
  // The shifted-register ADD form encodes an operand register of 31 as xzr
  // (zero), so when base is the stack pointer (sp, also register 31) it is
  // first copied into rd using the immediate ADD form (which does encode
  // Rn == 31 as sp). rd must be a general register distinct from index.
  void add_reg(Register rd, Register base, Register index, int scale);

  // rd = <sp>, used to move the stack pointer into a general scratch register
  // (mov rd, sp; the shifted ORR used by mov() would treat Rm == 31 as xzr).
  void mov_sp_to_reg(Register rd);

public:
  AArch64Assembler(CodeBuffer* code);

  // The x86 assembler nests its Condition enum, so generator code refers to
  // the conditions as Assembler::notZero etc.; expose the global enum and its
  // x86-style aliases as members for source compatibility.
  using Condition = ::Condition;
  static constexpr Condition zero = ::EQ;
  static constexpr Condition notZero = ::NE;
  static constexpr Condition equal = ::EQ;
  static constexpr Condition notEqual = ::NE;
  static constexpr Condition above = ::HI;
  static constexpr Condition aboveEqual = ::CS;
  static constexpr Condition below = ::CC;
  static constexpr Condition belowEqual = ::LS;
  static constexpr Condition greater = ::GT;
  static constexpr Condition greaterEqual = ::GE;
  static constexpr Condition less = ::LT;
  static constexpr Condition lessEqual = ::LE;
  static constexpr Condition negative = ::MI;
  static constexpr Condition positive = ::PL;
  static constexpr Condition overflow = ::VS;
  static constexpr Condition noOverflow = ::VC;
  static constexpr Condition carrySet = ::CS;
  static constexpr Condition carryClear = ::CC;

  void emit_quad_data(intptr_t data, relocInfo::relocType rtype); // 64-bit immediate + relocation

  // Absolute-address helpers (Address::absolute). AArch64 has no absolute
  // addressing mode, so an address is materialized via an inline literal:
  //   ldr scratch, [pc, #8];  b .+8;  .quad <address>
  // load_absolute_address  -> scratch = <address>
  // load_absolute_value    -> scratch = *<address>
  void load_absolute_address(Register scratch, Address src);
  void load_absolute_value(Register scratch, Address src);

  // Logical (shifted register)
  void and_(Register rd, Register rn, Register rm, ShiftType shift = LSL, int amt = 0, RegisterSize size = sz_64);
  void orr(Register rd, Register rn, Register rm, ShiftType shift = LSL, int amt = 0, RegisterSize size = sz_64);
  void eor(Register rd, Register rn, Register rm, ShiftType shift = LSL, int amt = 0, RegisterSize size = sz_64);
  void ands(Register rd, Register rn, Register rm, ShiftType shift = LSL, int amt = 0, RegisterSize size = sz_64);
  void bic(Register rd, Register rn, Register rm, ShiftType shift = LSL, int amt = 0, RegisterSize size = sz_64);
  void orn(Register rd, Register rn, Register rm, ShiftType shift = LSL, int amt = 0, RegisterSize size = sz_64);
  void eon(Register rd, Register rn, Register rm, ShiftType shift = LSL, int amt = 0, RegisterSize size = sz_64);
  void bics(Register rd, Register rn, Register rm, ShiftType shift = LSL, int amt = 0, RegisterSize size = sz_64);
  void tst(Register rn, Register rm, ShiftType shift = LSL, int amt = 0, RegisterSize size = sz_64); // ands xzr, rn, rm

  // Add/sub (shifted register)
  void add(Register rd, Register rn, Register rm, ShiftType shift = LSL, int amt = 0, RegisterSize size = sz_64);
  void sub(Register rd, Register rn, Register rm, ShiftType shift = LSL, int amt = 0, RegisterSize size = sz_64);
  void adds(Register rd, Register rn, Register rm, ShiftType shift = LSL, int amt = 0, RegisterSize size = sz_64);
  void subs(Register rd, Register rn, Register rm, ShiftType shift = LSL, int amt = 0, RegisterSize size = sz_64);
  void neg(Register rd, Register rm, ShiftType shift = LSL, int amt = 0, RegisterSize size = sz_64); // sub rd, xzr, rm
  void negs(Register rd, Register rm, ShiftType shift = LSL, int amt = 0,
            RegisterSize size = sz_64); // subs rd, xzr, rm
  void cmp(Register rn, Register rm, ShiftType shift = LSL, int amt = 0, RegisterSize size = sz_64); // subs xzr, rn, rm
  void cmn(Register rn, Register rm, ShiftType shift = LSL, int amt = 0, RegisterSize size = sz_64); // adds xzr, rn, rm

  // Add/sub (immediate)
  void add(Register rd, Register rn, int imm12, int sh = 0, RegisterSize size = sz_64);
  void sub(Register rd, Register rn, int imm12, int sh = 0, RegisterSize size = sz_64);
  void adds(Register rd, Register rn, int imm12, int sh = 0, RegisterSize size = sz_64);
  void subs(Register rd, Register rn, int imm12, int sh = 0, RegisterSize size = sz_64);
  void cmp(Register rn, int imm12, int sh = 0, RegisterSize size = sz_64); // subs xzr, rn, imm
  void cmn(Register rn, int imm12, int sh = 0, RegisterSize size = sz_64); // adds xzr, rn, imm

  // Logical (immediate)
  void and_(Register rd, Register rn, uint64_t imm, RegisterSize size = sz_64);
  void orr(Register rd, Register rn, uint64_t imm, RegisterSize size = sz_64);
  void eor(Register rd, Register rn, uint64_t imm, RegisterSize size = sz_64);
  void ands(Register rd, Register rn, uint64_t imm, RegisterSize size = sz_64);
  void tst(Register rn, uint64_t imm, RegisterSize size = sz_64); // ands xzr, rn, imm

  // Bitfield operations
  void lsl(Register rd, Register rn, int shift, RegisterSize size = sz_64);
  void lsr(Register rd, Register rn, int shift, RegisterSize size = sz_64);
  void asr(Register rd, Register rn, int shift, RegisterSize size = sz_64);
  void ubfx(Register rd, Register rn, int lsb, int width, RegisterSize size = sz_64);
  void sbfx(Register rd, Register rn, int lsb, int width, RegisterSize size = sz_64);
  void ubfiz(Register rd, Register rn, int lsb, int width, RegisterSize size = sz_64);
  void sbfiz(Register rd, Register rn, int lsb, int width, RegisterSize size = sz_64);
  void bfi(Register rd, Register rn, int lsb, int width, RegisterSize size = sz_64);
  void bfxil(Register rd, Register rn, int lsb, int width, RegisterSize size = sz_64);
  void uxtb(Register rd, Register rn, RegisterSize size = sz_32); // zero-extend byte
  void uxth(Register rd, Register rn, RegisterSize size = sz_32); // zero-extend halfword
  void uxtw(Register rd, Register rn); // zero-extend word (64-bit form)
  void sxtb(Register rd, Register rn, RegisterSize size = sz_32); // sign-extend byte
  void sxth(Register rd, Register rn, RegisterSize size = sz_32); // sign-extend halfword
  void sxtw(Register rd, Register rn); // sign-extend word (64-bit form)

  // Data-processing register (2 source)
  void lslv(Register rd, Register rn, Register rm, RegisterSize size = sz_64);
  void lsrv(Register rd, Register rn, Register rm, RegisterSize size = sz_64);
  void asrv(Register rd, Register rn, Register rm, RegisterSize size = sz_64);
  void rorv(Register rd, Register rn, Register rm, RegisterSize size = sz_64);
  void udiv(Register rd, Register rn, Register rm, RegisterSize size = sz_64);
  void sdiv(Register rd, Register rn, Register rm, RegisterSize size = sz_64);

  // Multiply
  void madd(Register rd, Register rn, Register rm, Register ra, RegisterSize size = sz_64);
  void mul(Register rd, Register rn, Register rm, RegisterSize size = sz_64); // madd rd, rn, rm, xzr
  void smulh(Register rd, Register rn, Register rm); // signed high half of rn * rm
  void msub(Register rd, Register rn, Register rm, Register ra, RegisterSize size = sz_64); // rd = ra - rn * rm
  void cset(Register rd, Condition cc); // rd = cc ? 1 : 0 (csinc rd, xzr, xzr, !cc)

  // Moves (move wide)
  void movz(Register rd, int imm16, int hw = 0, RegisterSize size = sz_64);
  void movn(Register rd, int imm16, int hw = 0, RegisterSize size = sz_64);
  void movk(Register rd, int imm16, int hw = 0, RegisterSize size = sz_64);
  void mov(Register rd, Register rm, RegisterSize size = sz_64); // orr rd, xzr, rm

  // Memory (base + unsigned imm12 / unscaled imm9 / register offset)
  void ldr(Register rt, Address adr);
  void str(Register rt, Address adr);
  void ldr_w(Register rt, Address adr);
  void str_w(Register rt, Address adr);
  void ldr_b(Register rt, Address adr);
  void str_b(Register rt, Address adr);
  void ldr_h(Register rt, Address adr);
  void str_h(Register rt, Address adr);
  void ldur(Register rt, Address adr, RegisterSize size = sz_64);
  void stur(Register rt, Address adr, RegisterSize size = sz_64);
  void ldur(FloatRegister ft, Address adr, RegisterSize size = sz_64); // unscaled FP
  void stur(FloatRegister ft, Address adr, RegisterSize size = sz_64);

  // Memory (pre-/post-index, unscaled imm9)
  void ldr_pre(Register rt, Register rn, int imm9);
  void str_pre(Register rt, Register rn, int imm9);
  void ldr_post(Register rt, Register rn, int imm9);
  void str_post(Register rt, Register rn, int imm9);

  // Load/store pair (offset in bytes, element size 8 (X) / 4 (W) bytes)
  void ldp(Register rt1, Register rt2, Register rn, int offset, RegisterSize size = sz_64);
  void stp(Register rt1, Register rt2, Register rn, int offset, RegisterSize size = sz_64);
  void ldp_pre(Register rt1, Register rt2, Register rn, int offset, RegisterSize size = sz_64);
  void stp_pre(Register rt1, Register rt2, Register rn, int offset, RegisterSize size = sz_64);
  void ldp_post(Register rt1, Register rt2, Register rn, int offset, RegisterSize size = sz_64);
  void stp_post(Register rt1, Register rt2, Register rn, int offset, RegisterSize size = sz_64);

  // Load literal (PC-relative)
  void ldr(Register rt, Label& L); // 64-bit literal
  void ldr_w(Register rt, Label& L); // 32-bit literal
  void ldr(FloatRegister ft, Label& L); // double literal
  void ldr_s(FloatRegister ft, Label& L); // single literal

  // Branch, branch-link and compare/branch (labels)
  void b(Label& L);
  void bl(Label& L);
  void b(Condition cc, Label& L);
  void cbz(Register rt, Label& L, RegisterSize size = sz_64);
  void cbnz(Register rt, Label& L, RegisterSize size = sz_64);
  void tbz(Register rt, int bit, Label& L);
  void tbnz(Register rt, int bit, Label& L);
  void adr(Register rd, Label& L);

  // Calls / jumps (register and absolute targets)
  void call(Label& L);
  void call(char* entry, relocInfo::relocType rtype);
  void call(Register reg);
  void jmp(Label& L);
  void jmp(char* entry, relocInfo::relocType rtype);
  void jmp(Register reg);

  // Direct branch to an absolute target (via a literal address)
  void b(char* entry, relocInfo::relocType rtype);
  void bl(char* entry, relocInfo::relocType rtype);

  // System / miscellaneous
  void br(Register rn);
  void blr(Register rn);
  void ret(Register rn = lr);
  void nop();

  // Floating-point arithmetic (scalar)
  void fadd(FloatRegister fd, FloatRegister fn, FloatRegister fm, RegisterSize size = sz_64);
  void fsub(FloatRegister fd, FloatRegister fn, FloatRegister fm, RegisterSize size = sz_64);
  void fmul(FloatRegister fd, FloatRegister fn, FloatRegister fm, RegisterSize size = sz_64);
  void fdiv(FloatRegister fd, FloatRegister fn, FloatRegister fm, RegisterSize size = sz_64);
  void fcmp(FloatRegister fn, FloatRegister fm, RegisterSize size = sz_64);
  void fcmp0(FloatRegister fn, RegisterSize size = sz_64); // fcmp fn, #0.0
  void fmov(FloatRegister fd, FloatRegister fn, RegisterSize size = sz_64);
  void fabs(FloatRegister fd, FloatRegister fn, RegisterSize size = sz_64);
  void fneg(FloatRegister fd, FloatRegister fn, RegisterSize size = sz_64);
  void fsqrt(FloatRegister fd, FloatRegister fn, RegisterSize size = sz_64);
  void fcvtzs(Register rd, FloatRegister fn, RegisterSize size = sz_64,
              RegisterSize dstSize = sz_64); // size = FP size, dstSize = integer size

  // Integer -> floating-point conversion; size = FP (d/s) size, srcSize = integer (w/x) size
  void scvtf(FloatRegister fd, Register rn, RegisterSize size = sz_64, RegisterSize srcSize = sz_64);
  void fmov(FloatRegister fd, Register rn, RegisterSize size = sz_64); // fmov d/s, x/w (fmov d, xzr is +0.0)

  // Conditional select: rd = cc ? rn : rm
  void csel(Register rd, Register rn, Register rm, Condition cc, RegisterSize size = sz_64);

  // Floating-point load/store
  void ldr(FloatRegister ft, Address adr); // 64-bit (double)
  void str(FloatRegister ft, Address adr);
  void ldr_s(FloatRegister ft, Address adr); // 32-bit (single)
  void str_s(FloatRegister ft, Address adr);
  void ldr_pre(FloatRegister ft, Register rn, int imm9);
  void str_pre(FloatRegister ft, Register rn, int imm9);
  void ldr_post(FloatRegister ft, Register rn, int imm9);
  void str_post(FloatRegister ft, Register rn, int imm9);

  // Load/Store of addresses used by the compiler (see asm/mapping.hpp)
  void Load(Register base, intptr_t disp, Register dst) { ldr(dst, Address(base, disp)); }
  void Store(Register src, Register base, intptr_t disp) { str(src, Address(base, disp)); }

  enum Constants {
    sizeOfCall = 4 // length of a call (blr) instruction in bytes
  };

  // Stack ops used with base-class assemblers (frame setup in the code
  // generator); the macro layer's pushl/popl are the oop-slot variants.
  void pushl(Register src); // str src, [sp, #-8]!
  void popl(Register dst); // ldr dst, [sp], #8
};

// AArch64MacroAssembler extends AArch64Assembler by a few macros used for
// generating the interpreter and for compiled code.

class AArch64MacroAssembler : public AArch64Assembler {
public:
  AArch64MacroAssembler(CodeBuffer* code) : AArch64Assembler(code), _float_depth(0) {}

  // Alignment
  void align(int modulus);

  // Moves
  void mov(Register rd, intptr_t imm); // orr (logical imm) or movz/movk sequence
  void mov(Register rd, Register rm); // orr rd, xzr, rm

  // Stack frame operations
  void push(Register src); // str src, [sp, #-16]!  (keeps sp 16-byte aligned)
  void pop(Register dst); // ldr dst, [sp], #16

  // test optimized for the logical immediate form
  void test(Register rn, uint64_t imm) { ands(xzr, rn, imm); }

  // ------------------------------------------------------------------
  // x86-compatibility instruction set used by the interpreter generator
  // (see vm/interpreter/interpreter.cpp). Each method expands to one or
  // more AArch64 instructions with the SAME effect as the x86 operation
  // on a 64-bit VM with 8-byte oop slots; "l"/"q" suffixes are ignored
  // (AArch64 has no 32-bit push or 32-bit-only registers - oops, smis
  // and bytecode pointers are all manipulated at 64-bit width).
  //
  // Moves
  void movl(Register dst, int imm32);
  void movl(Register dst, oop obj); // embed oop value
  void movl(Register dst, Register src) { mov(dst, src); }
  void movl(Register dst, Address src); // ldr/ldur; absolute via literal
  void movl(Address dst, Register src); // str/stur; absolute via temp reg
  void movl(Address dst, int imm32); // materialize imm, then store
  void movl(Address dst, oop obj); // embed oop value, then store
  void movq(Register dst, intptr_t imm) { mov(dst, imm); }
  void movq(Register dst, Register src) { movl(dst, src); }
  void movq(Register dst, Address src) { movl(dst, src); }
  void movq(Address dst, Register src) { movl(dst, src); }
  void movq(Address dst, intptr_t imm) { movl(dst, imm); }
  void movq(Address dst, oop obj) { movl(dst, obj); }
  void movb(Register dst, Address src); // zero-extending byte load
  void movb(Address dst, Register src);
  void movb(Address dst, int imm8);

  // Stack (8-byte oop slots; sp may become 8-byte aligned - call_C and the
  // C-call helpers re-align sp before a blr)
  void pushl(Register src);
  void pushl(Address src);
  void pushl(int imm32); // sign-extended 64-bit push
  void pushl(oop obj); // pointer-sized push
  void popl(Register dst);
  void popl(Address dst); // pop top of stack into memory
  void pushq(Register src) { pushl(src); }
  void pushq(int imm32) { pushl(imm32); }
  void pushq(oop obj) { pushl(obj); }
  void pushad(); // save eax..esi (interpreter registers)
  void popad(); // restore eax..esi

  // Arithmetic (64-bit; smis and oops are 64-bit values)
  void addl(Register dst, int imm);
  void addl(Register dst, Register src);
  void addl(Register dst, Address src);
  void addl(Address dst, int imm); // read-modify-write
  void addq(Register dst, int imm) { addl(dst, imm); }
  void addq(Register dst, Register src) { addl(dst, src); }
  void addq(Register dst, Address src) { addl(dst, src); }
  void subl(Register dst, int imm);
  void subl(Register dst, Register src);
  void subl(Register dst, Address src);
  void incl(Register reg) { add(reg, reg, 1); }
  void decl(Register reg) { subs(reg, reg, 1); }
  void decb(Register reg) { subs(reg, reg, 1); } // byte count decrement (interpreter)
  void incb(Register reg) { add(reg, reg, 1); }
  void incq(Register reg) { incl(reg); }
  void decq(Register reg) { decl(reg); }
  void incl(Address dst); // read-modify-write (+1)
  void incq(Address dst) { incl(dst); }
  void decl(Address dst); // read-modify-write (-1)
  void decq(Address dst) { decl(dst); }
  void negl(Register reg) { neg(reg, reg); }
  void negq(Register reg) { neg(reg, reg); }
  void notl(Register reg);
  void imull(Register dst, Register src); // mul (64-bit)
  void imull(Register dst, int imm);
  void imull(Register dst, Register src1, Register src2) { mul(dst, src1, src2); }
  void imull(Register dst, Register src, int value); // dst := src * value
  void imull(Register src); // eax := eax * src; Z = 1 iff no overflow
  void imull(int imm); // eax := eax * imm; Z = 1 iff no overflow
  void cdq(); // edx := sign-extend(eax)
  void idivl(Register src); // eax := edx:eax / src, edx := remainder; Z = 1 iff no overflow

  // Logical
  void andl(Register dst, int imm); // 32-bit x86 semantics: zero-extended mask
  void andl(Register dst, Register src) { and_(dst, dst, src); }
  void andq(Register dst, Register src) { andl(dst, src); }
  void andq(Register dst, intptr_t imm) { and_(dst, dst, (uint64_t)imm); }
  void orl(Register dst, Register src) { orr(dst, dst, src); }
  void orl(Register dst, int imm); // 32-bit x86 semantics: zero-extended mask
  void orl(Register dst, Address src);
  void xorl(Register dst, Register src) { eor(dst, dst, src); }
  void xorl(Register dst, int imm); // 32-bit x86 semantics: zero-extended mask

  // Shifts. The single-argument forms use ecx (x11) as the shift count,
  // mirroring the x86 shift-by-CL idiom used by the interpreter.
  void shll(Register reg, int shift) { lsl(reg, reg, shift); }
  void sarl(Register reg, int shift) { asr(reg, reg, shift); }
  void shrl(Register reg, int shift) { lsr(reg, reg, shift); }
  void shll(Register reg) { lslv(reg, reg, ecx); }
  void sarl(Register reg) { asrv(reg, reg, ecx); }
  void shrl(Register reg) { lsrv(reg, reg, ecx); }

  // Address computation (leal/leaq both compute the 64-bit effective address)
  void leal(Register dst, Address src);
  void leaq(Register dst, Address src) { leal(dst, src); }
  // dst = x16, handling a stack-pointer destination (see leal).
  void write_leal_result(Register dst, Register x16_val);

  // Compare / test
  void cmpl(Register dst, int imm);
  void cmpl(Register dst, oop obj);
  void cmpl(Register dst, Register src);
  void cmpl(Register dst, Address src);
  void cmpl(Address dst, int imm);
  void cmpl(Address dst, oop obj);
  void cmpl(Address dst, Register src);
  void testl(Register reg, int imm);
  void testl(Register dst, Register src) { tst(dst, src); }
  void testb(Register reg, int imm);
  void jcc(Condition cc, Label& L) { b(cc, L); }
  void jcc(Condition cc, char* entry); // conditional jump to an absolute address
  void jcc(Condition cc, char* entry, relocInfo::relocType rtype);
  void jmp(Condition cc, char* entry) { jcc(cc, entry); }

  // Branch / call
  void jmp(Address dst); // computed jump (bytecode dispatch)
  void call(Address dst); // computed call
  void jmp(Label& L) { AArch64Assembler::jmp(L); }
  void call(Label& L) { AArch64Assembler::call(L); }
  void jmp(char* entry, relocInfo::relocType rtype) { AArch64Assembler::jmp(entry, rtype); }
  void call(char* entry, relocInfo::relocType rtype) { AArch64Assembler::call(entry, rtype); }
  // Re-expose the base-class register forms: declaring any `call`/`jmp`
  // overload above hides AArch64Assembler::call(Register)/jmp(Register),
  // which would silently make shared code like `masm->call(edx)` resolve to
  // call(Address) and dereference the register instead of branching to it.
  void call(Register reg) { AArch64Assembler::call(reg); }
  void jmp(Register reg) { AArch64Assembler::jmp(reg); }

  // Frame setup/teardown (matches the interpreter/compiled frame layout:
  // fp[0] = link, fp[1] = return address)
  void enter();
  void leave();

  // Misc
  void hlt(); // brk #0
  void int3() { hlt(); }
  void ret(int imm = 0); // ret (imm is the x86 pop count, ignored)

  // NLR inline-cache info. On x86 this emits a 5-byte marker that the frame
  // walker scans for; on AArch64 the runtime scanning is redesigned in the
  // retarget phase - for now emit a NOP placeholder (all referenced labels
  // are bound elsewhere).
  void ic_info(Label& L, int flags) { nop(); }

  // Runtime frame bookkeeping (used around C calls so NLRs can unwind)
  void set_last_Delta_frame_before_call();
  void set_last_Delta_frame_after_call(); // caller's sp/fp (return address in x30)
  void reset_last_Delta_frame();

  // Debugging: called by generate_call_inspector (debugging only)
  static void inspector(oop edi, oop esi, oop ebp, oop esp, oop ebx, oop edx, oop ecx, oop eax, char* eip);

  // ------------------------------------------------------------------
  // Float stack (x87-style, see asm/mapping_aarch64.hpp). The interpreter
  // models the x87 register stack with a generation-time depth counter
  // (_float_depth): st(i) (i = 0 is the top) maps to the callee-saved
  // double d(8 + _float_depth - 1 - i), so live floats survive the C calls
  // the interpreter makes between bytecodes. Every float bytecode leaves
  // the stack at the same depth it entered it with (the x87 model,
  // including the stale divisor left behind by fprem).
  void fld_d(Address src); // push double from memory
  void fstp_d(Address dst); // pop double to memory
  void fild_s(Address src); // push (double)(int) from memory
  void fpop(); // discard top of stack
  void push_float(); // reserve one float stack slot
  void pop_float(); // release one float stack slot
  void fldz(); // push +0.0
  void fld1(); // push 1.0
  void fabs(); // st(0) := |st(0)|
  void fchs(); // st(0) := -st(0)
  void fsqrt(); // st(0) := sqrt(st(0))
  void fmul(int i = 0); // st(0) := st(0) * st(i)
  void faddp(int i = 1); // st(i) := st(i) + st(0), pop
  void fsubp(int i = 1); // st(i) := st(i) - st(0), pop
  void fmulp(int i = 1); // st(i) := st(i) * st(0), pop
  void fdivp(int i = 1); // st(i) := st(i) / st(0), pop
  void fadd_d(Address src); // st(0) := st(0) + double [src]
  void fsub_d(Address src); // st(0) := st(0) - double [src]
  void fmul_d(Address src); // st(0) := st(0) * double [src]
  void fdiv_d(Address src); // st(0) := st(0) / double [src]
  void fxch(int i = 1); // swap st(0) and st(i)
  void fprem(); // st(0) := st(0) mod st(1), drop st(1)
  void ftst(); // compare st(0) with +0.0 (sets the x87-style flags)
  void fcompp(); // compare st(1) with st(0), pop twice
  void fnstsw_ax(); // materialize x87-style flags in eax (x13)
  void fwait(); // no-op
  static void fpu_mask_and_cond_for(Condition cc, int& mask, Condition& cond);

  // Float-stack depth management. The shared interpreter/Floats code
  // (floats.cpp, interpreter.cpp) sets the generation-time depth so that the
  // st(i) -> d(8 + depth - 1 - i) mapping matches the runtime depth when the
  // code is entered (bytecodes at depth 0, function-table entries at the
  // depth their caller pushes). No-op on x86 (hardware x87 stack).
  int float_depth() const { return _float_depth; }
  void set_float_depth(int d) { _float_depth = d; }

  // The x87-style forms above hide the base-class 3-operand FP arithmetic;
  // keep both usable.
  using AArch64Assembler::fmul;
  using AArch64Assembler::fabs;
  using AArch64Assembler::fsqrt;

  // Compatibility with the old assembler interface (see assembler_x86.hpp).
  // These generate the interpreter/compiler runtime calls and are not part of
  // the encoder work; they are declared for API parity and abort until the JIT
  // retarget phase implements them.
  void call_C(Label& L);
  void call_C(Label& L, Label& nlrTestPoint);
  void call_C(char* entry, relocInfo::relocType rtype);
  void call_C(char* entry, relocInfo::relocType rtype, Label& nlrTestPoint);
  void call_C(Register entry);
  void call_C(Register entry, Label& nlrTestPoint);
  void call_C(char* entry, Register arg1);
  void call_C(char* entry, Register arg1, Register arg2);
  void call_C(char* entry, Register arg1, Register arg2, Register arg3);
  void call_C(char* entry, Register arg1, Register arg2, Register arg3, Register arg4);
  void store_check(Register obj, Register tmp);

  // Support for inlined data (compiler)
  void inline_oop(oop o); // embed an oop with an oop_type relocation

private:
  int _float_depth; // current x87-style float stack depth
  FloatRegister st_reg(int i) const; // st(i) -> d(8 + depth - 1 - i)

  // In the shifted-register add/sub/logical encodings a source register 31 is
  // decoded as xzr (zero), not sp; only Rd, and addsub_imm sources, alias 31
  // to sp. Since xzr and sp are both Register(31), this is resolved at the
  // macro layer where esp is explicit: materialize an esp source in a scratch
  // GPR (add xN, sp, #0) before emitting the shifted-form operation, picking a
  // scratch that does not clobber the other live operand.
  Register sp_source(Register reg, Register avoid);
  void addsub_with_sp(Register rd, Register rn, Register rm, bool isSub, ShiftType shift, int amt);
  void cmp_with_sp(Register rn, Register rm);
};
#endif // _ASSEMBLER_AARCH64_HPP
