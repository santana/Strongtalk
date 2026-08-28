/*
Copyright (c) 2026, Gerardo Santana Gomez Garrido.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// The x86 assembler backend. This is the only backend implemented so far;
// it is selected by "asm/assembler.hpp" (via the Assembler/MacroAssembler
// aliases). A new backend would follow the same pattern: extend
// AbstractAssembler, implement the instruction set and the backend-specific
// label fixup, and hook it up in assembler.hpp.

#ifndef _ASSEMBLER_X86_HPP
#define _ASSEMBLER_X86_HPP

#include "asm/abstractAssembler.hpp"
#include "code/relocInfo.hpp"
#include "memory/allocation.hpp"
#include "oops/oopsHierarchy.hpp"

// The x86 assembler encodes both 32-bit and 64-bit instructions. The word
// size is selected at compile time from the platform pointer size, so that
// -m32 / -m64 (or LP64) builds automatically pick the right encoding. The
// 32-bit path can be forced on a 64-bit host by defining DELTA_X86_32 (the
// encoder tests use this to exercise both encoders). DELTA_X86_64 is defined
// on 64-bit builds, DELTA_X86_32 on 32-bit builds.
//
// On a 64-bit build all pointer-sized operations (register moves, stack
// pushes, arithmetic) must use the explicit 64-bit instruction forms (the
// *q variants). The 32-bit forms are still available and emit true 32-bit
// instructions.

#if defined(DELTA_X86_32) && defined(DELTA_X86_64)
#error "define only one of DELTA_X86_32 / DELTA_X86_64"
#endif

#if !defined(DELTA_X86_32) && !defined(DELTA_X86_64)
#if defined(__x86_64__) || defined(__aarch64__) || defined(__powerpc64__) || defined(__LP64__) || defined(_LP64) ||    \
  defined(_M_X64)
#define DELTA_X86_64 1
#else
#define DELTA_X86_32 1
#endif
#endif

// Both macros are always defined (as 0/1) so that tests like
// "if (DELTA_X86_64)" work on either build.
#ifndef DELTA_X86_32
#define DELTA_X86_32 0
#endif
#ifndef DELTA_X86_64
#define DELTA_X86_64 0
#endif

#if DELTA_X86_64
const int BytesPerNativeWord = 8; // size of a native word (pointer) in bytes
const int nofRegisters = 16; // total number of registers
#else
const int BytesPerNativeWord = 4; // size of a native word (pointer) in bytes
const int nofRegisters = 8; // total number of registers
#endif

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
  bool hasByteRegister() const { return 0 <= _number && _number <= 3; }

  // comparison
  friend bool operator==(Register x, Register y) { return x._number == y._number; }
  friend bool operator!=(Register x, Register y) { return x._number != y._number; }

  // debugging
  char* name() const;
};

// Available registers
const Register eax = Register(0, ' ');
const Register ecx = Register(1, ' ');
const Register edx = Register(2, ' ');
const Register ebx = Register(3, ' ');
const Register esp = Register(4, ' ');
const Register ebp = Register(5, ' ');
const Register esi = Register(6, ' ');
const Register edi = Register(7, ' ');

#if DELTA_X86_64
const Register r8 = Register(8, ' ');
const Register r9 = Register(9, ' ');
const Register r10 = Register(10, ' ');
const Register r11 = Register(11, ' ');
const Register r12 = Register(12, ' ');
const Register r13 = Register(13, ' ');
const Register r14 = Register(14, ' ');
const Register r15 = Register(15, ' ');
#endif

const Register noreg; // Dummy register used in Load, LoadAddr, and Store.

// Address operands for assembler

class Address : public ValueObj {
public:
  enum ScaleFactor {
    no_scale = -1,
    times_1 = 0,
    times_2 = 1,
    times_4 = 2,
    times_8 = 3
  };

private:
  Register _base;
  Register _index;
  ScaleFactor _scale;
  intptr_t _disp;
  relocInfo::relocType _rtype;

public:
  Address();
  Address(intptr_t disp, relocInfo::relocType rtype);
  Address(Register base, intptr_t disp = 0, relocInfo::relocType rtype = relocInfo::none);
  Address(Register base, Register index, ScaleFactor scale, intptr_t disp = 0,
          relocInfo::relocType rtype = relocInfo::none);

  friend class X86Assembler;
};

// The x86 assembler. It extends the common AbstractAssembler with the
// x86 instruction set and the x86 specific label fixup machinery.

class X86Assembler : public AbstractAssembler {
protected:
  void emit_arith_b(int op1, int op2, Register dst, int imm8);

  void emit_arith(int op1, int op2, Register dst, int imm32, bool rex_w = false);
  void emit_arith(int op1, int op2, Register dst, oop obj);
  void emit_arith(int op1, int op2, Register dst, Register src, bool rex_w = false);

  // REX prefix support (x86-64). rex_bits() computes the REX.R/X/B bits
  // required for the operand registers (reg -> REX.R, index -> REX.X,
  // base -> REX.B); emit_rex() writes the prefix byte (0x40 base) before an
  // opcode when non-zero; emit_rex_w() additionally sets the REX.W bit that
  // marks an instruction operating on 64-bit operands. On 32-bit builds
  // rex_bits() is always 0 and emit_rex() emits nothing, so the 32-bit
  // encoding is byte-identical to the pre-REX encoder.
  int rex_bits(Register reg, Register base = noreg, Register index = noreg);
  void emit_rex(int rex);
  void emit_rex_w(Register reg = noreg, Register base = noreg, Register index = noreg);

  void emit_quad_data(intptr_t data, relocInfo::relocType rtype); // 64-bit immediate + relocation

  int _last_rex; // the most recently emitted REX prefix byte (0 if none)

  void emit_operand(Register reg, Register base, Register index, Address::ScaleFactor scale, intptr_t disp,
                    relocInfo::relocType rtype);
  void emit_operand(Register reg, Address adr);

  void emit_farith(int b1, int b2, int i);

  // x86 label fixup
  void print(Label& L);
  void bind_to(Label& L, int pos);
  void link_to(Label& L, Label& appendix);

public:
  enum Condition {
    zero = 0x4,
    notZero = 0x5,
    equal = 0x4,
    notEqual = 0x5,
    less = 0xc,
    lessEqual = 0xe,
    greater = 0xf,
    greaterEqual = 0xd,
    below = 0x2,
    belowEqual = 0x6,
    above = 0x7,
    aboveEqual = 0x3,
    overflow = 0x0,
    noOverflow = 0x1,
    carrySet = 0x2,
    carryClear = 0x3,
    negative = 0x8,
    positive = 0x9,
  };

  enum Constants {
    sizeOfCall = 5 // length of call instruction in bytes
  };

  X86Assembler(CodeBuffer* code);

  // finalize() is inherited from AbstractAssembler.

  // Stack
  // Note: pushad/popad exist only in 32-bit mode; on a 64-bit build they
  // abort (there is no 64-bit pushad/popad - use pushq/popq or individual
  // register pushes).
  void pushad();
  void popad();

  void pushl(int imm32);
  void pushl(oop obj);
  void pushl(Register src);
  void pushl(Address src);

  void popl(Register dst);
  void popl(Address dst);

  // 64-bit stack operations (x86-64 only)
  void pushq(Register src);
  void popq(Register dst);

  // Moves
  void movb(Register dst, Address src);
  void movb(Address dst, int imm8);
  void movb(Address dst, Register src);

  void movw(Register dst, Address src);
  void movw(Address dst, Register src);

  void movl(Register dst, int imm32);
  void movl(Register dst, oop obj);
  void movl(Register dst, Register src);
  void movl(Register dst, Address src);

  void movl(Address dst, int imm32);
  void movl(Address dst, oop obj);
  void movl(Address dst, Register src);

  // 64-bit moves (x86-64 only). movq(dst, imm/oop) is a full 64-bit
  // immediate load (movabs); movq(dst, Address) loads 8 bytes.
  void movq(Register dst, Register src);
  void movq(Register dst, Address src);
  void movq(Address dst, Register src);
  void movq(Address dst, int imm32); // sign-extended 32-bit immediate
  void movq(Register dst, intptr_t imm); // full 64-bit immediate (movabs)
  void movq(Register dst, oop obj); // full 64-bit immediate (movabs)

  void movsxq(Register dst, Register src); // sign-extend 32-bit src into 64-bit dst

  void movsxb(Register dst, Address src);
  void movsxb(Register dst, Register src);

  void movsxw(Register dst, Address src);
  void movsxw(Register dst, Register src);

  // Conditional moves (P6 only)
  void cmovccl(Condition cc, Register dst, int imm32);
  void cmovccl(Condition cc, Register dst, oop obj);
  void cmovccl(Condition cc, Register dst, Register src);
  void cmovccl(Condition cc, Register dst, Address src);

  // Arithmetics
  void adcl(Register dst, int imm32);
  void adcl(Register dst, Register src);

  void addl(Address dst, int imm32);
  void addl(Register dst, int imm32);
  void addl(Register dst, Register src);
  void addl(Register dst, Address src);

  void andl(Register dst, int imm32);
  void andl(Register dst, Register src);

  void cmpl(Address dst, int imm32);
  void cmpl(Address dst, oop obj);
  void cmpl(Register dst, int imm32);
  void cmpl(Register dst, oop obj);
  void cmpl(Register dst, Register src);
  void cmpl(Register dst, Address src);

  void decb(Register dst);
  void decl(Register dst);
  void decl(Address dst);

  void idivl(Register src);

  void imull(Register src);
  void imull(Register dst, Register src);
  void imull(Register dst, Register src, int value);

  void incl(Register dst);
  void incl(Address dst);

  void leal(Register dst, Address src);

  void mull(Register src);

  void negl(Register dst);

  void notl(Register dst);

  void orl(Register dst, int imm32);
  void orl(Register dst, Register src);
  void orl(Register dst, Address src);

  void rcll(Register dst, int imm8);

  void sarl(Register dst, int imm8);
  void sarl(Register dst);

  void sbbl(Register dst, int imm32);
  void sbbl(Register dst, Register src);

  void shldl(Register dst, Register src);

  void shll(Register dst, int imm8);
  void shll(Register dst);

  void shrdl(Register dst, Register src);

  void shrl(Register dst, int imm8);
  void shrl(Register dst);

  void subl(Register dst, int imm32);
  void subl(Register dst, Register src);
  void subl(Register dst, Address src);

  void testb(Register dst, int imm8);
  void testl(Register dst, int imm32);
  void testl(Register dst, Register src);

  void xorl(Register dst, int imm32);
  void xorl(Register dst, Register src);

  // 64-bit arithmetic (x86-64 only)
  void addq(Address dst, int imm32);
  void addq(Register dst, int imm32);
  void addq(Register dst, Register src);
  void addq(Register dst, Address src);

  void andq(Register dst, int imm32);
  void andq(Register dst, Register src);

  void cmpq(Address dst, int imm32);
  void cmpq(Register dst, int imm32);
  void cmpq(Register dst, Register src);
  void cmpq(Register dst, Address src);

  void decq(Register dst);
  void decq(Address dst);

  void incq(Register dst);
  void incq(Address dst);

  void leaq(Register dst, Address src);

  void idivq(Register src);
  void imulq(Register dst, Register src);
  void imulq(Register dst, Register src, int value);

  void mulq(Register src);

  void negq(Register dst);
  void notq(Register dst);

  void orq(Register dst, int imm32);
  void orq(Register dst, Register src);
  void orq(Register dst, Address src);

  void sarq(Register dst, int imm8);
  void sarq(Register dst); // CL-shift (64-bit)
  void shlq(Register dst, int imm8);
  void shlq(Register dst); // CL-shift (64-bit)
  void shrq(Register dst, int imm8);
  void shrq(Register dst); // CL-shift (64-bit)

  void subq(Register dst, int imm32);
  void subq(Register dst, Register src);
  void subq(Register dst, Address src);

  void testq(Register dst, int imm32);
  void testq(Register dst, Register src);

  void xorq(Register dst, int imm32);
  void xorq(Register dst, Register src);

  // Miscellaneous
  void cdq();
  void cqo(); // sign-extend rax into rdx:rax (x86-64)
  void hlt();
  void int3();
  void nop();
  void ret(int imm16 = 0);

  // Labels

  void bind(Label& L); // binds an unbound label L to the current code position
  virtual void merge(Label& L, Label& with); // merges L and with, L is the merged label

  // Calls
  void call(Label& L);
  void call(char* entry, relocInfo::relocType rtype);
  void call(Register reg);
  void call(Address adr);

  // Jumps
  void jmp(char* entry, relocInfo::relocType rtype);
  void jmp(Register reg);
  void jmp(Address adr);

  // Label operations & relative jumps (PPUM Appendix D)
  void jmp(Label& L); // unconditional jump to L

  // jccI is the generic conditional branch generator to run-
  // time routines, jcc is used for branches to labels. jcc
  // takes a branch opcode (cc) and a label (L) and generates
  // either a backward branch or a forward branch and links it
  // to the label fixup chain. Usage:
  //
  // Label L;		// unbound label
  // jcc(cc, L);	// forward branch to unbound label
  // bind(L);		// bind label to the current pc
  // jcc(cc, L);	// backward branch to bound label
  // bind(L);		// illegal: a label may be bound only once
  //
  // Note: The same Label can be used for forward and backward branches
  // but it may be bound only once.

  void jcc(Condition cc, char* dst, relocInfo::relocType rtype = relocInfo::runtime_call_type);
  void jcc(Condition cc, Label& L);

  // Support for inline cache information (see also IC_Info)
  void ic_info(Label& L, int flags);

  // Floating-point operations
  void fld1();
  void fldz();

  // %note: _s 32 bits, _d 64 bits
  void fld_s(Address adr);
  void fld_d(Address adr);

  void fstp_s(Address adr);
  void fstp_d(Address adr);

  void fild_s(Address adr);
  void fild_d(Address adr);

  void fistp_s(Address adr);
  void fistp_d(Address adr);

  void fabs();
  void fchs();

  void fadd_d(Address adr);
  void fsub_d(Address adr);
  void fmul_d(Address adr);
  void fdiv_d(Address adr);

  void fadd(int i);
  void fsub(int i);
  void fmul(int i);
  void fdiv(int i);

  void faddp(int i = 1);
  void fsubp(int i = 1);
  void fsubrp(int i = 1);
  void fmulp(int i = 1);
  void fdivp(int i = 1);
  void fprem();
  void fprem1();

  void fxch(int i = 1);
  void fincstp();
  void ffree(int i = 0);

  void ftst();
  void fcompp();
  void fnstsw_ax();
  void fwait();

  // For compatibility with old assembler only - should be removed at some point
  void Load(Register base, intptr_t disp, Register dst) { movl(dst, Address(base, disp)); }
  void Store(Register src, Register base, intptr_t disp) { movl(Address(base, disp), src); }
};

// X86MacroAssembler extends X86Assembler by a few macros used for
// generating the interpreter and for compiled code.

class X86MacroAssembler : public X86Assembler {
public:
  X86MacroAssembler(CodeBuffer* code) : X86Assembler(code) {}

#if DELTA_X86_64
  // On 64-bit x86, oops are 64-bit pointers.  The low-level X86Assembler
  // *l methods emit 32-bit instructions (no REX.W), which silently
  // truncate pointers to 32 bits and zero-extend.  Override the forms
  // used by the allocation stubs and interpreter to emit 64-bit *q
  // instructions instead.
  using X86Assembler::movl;
  void movl(Register dst, Address src) { movq(dst, src); }
  void movl(Address dst, Register src) { movq(dst, src); }
  void movl(Register dst, Register src) { movq(dst, src); }
  // movl(Address, int) must also store 64 bits; materialise via r10 (temp3).
  void movl(Address dst, int imm32) {
    movl(r10, imm32); // zero-extends to 64-bit
    movq(dst, r10); // 64-bit store
  }

  using X86Assembler::addl;
  void addl(Register dst, int imm) { addq(dst, imm); }
  void addl(Register dst, Register src) { addq(dst, src); }
  void addl(Register dst, Address src) { addq(dst, src); }

  using X86Assembler::subl;
  void subl(Register dst, int imm) { subq(dst, imm); }
  void subl(Register dst, Register src) { subq(dst, src); }
  void subl(Register dst, Address src) { subq(dst, src); }

  using X86Assembler::leal;
  void leal(Register dst, Address src) { leaq(dst, src); }

  using X86Assembler::cmpl;
  void cmpl(Register dst, Register src) { cmpq(dst, src); }
  void cmpl(Register dst, Address src) { cmpq(dst, src); }

  using X86Assembler::andl;
  void andl(Register dst, int imm) { andq(dst, imm); }
  void andl(Register dst, Register src) { andq(dst, src); }

  using X86Assembler::orl;
  void orl(Register dst, int imm) { orq(dst, imm); }
  void orl(Register dst, Register src) { orq(dst, src); }
  void orl(Register dst, Address src) { orq(dst, src); }

  using X86Assembler::xorl;
  void xorl(Register dst, int imm) { xorq(dst, imm); }
  void xorl(Register dst, Register src) { xorq(dst, src); }

  using X86Assembler::shll;
  void shll(Register dst, int imm) { shlq(dst, imm); }
  void shll(Register dst) { shlq(dst); }

  using X86Assembler::sarl;
  void sarl(Register dst, int imm) { sarq(dst, imm); }
  void sarl(Register dst) { sarq(dst); }

  using X86Assembler::shrl;
  void shrl(Register dst, int imm) { shrq(dst, imm); }
  void shrl(Register dst) { shrq(dst); }

  using X86Assembler::incl;
  void incl(Register dst) { incq(dst); }

  using X86Assembler::decl;
  void decl(Register dst) { decq(dst); }

  // 32-bit load/store that bypass the movl→movq override.
  // Use for genuine 32-bit fields (e.g., invocation counter padding
  // must NOT be touched; a 64-bit read picks up garbage and makes
  // the counter always look overflowed).
  void movl_32(Register dst, const Address& src) { X86Assembler::movl(dst, src); }
  void movl_32(const Address& dst, Register src) { X86Assembler::movl(dst, src); }

#endif

  // Alignment
  void align(int modulus);

  // Test-Instructions optimized for length
  void test(Register dst, int imm8); // use testb if possible, testl otherwise

  // Stack frame operations
  void enter();
  void leave();

  // Support for inlined data

  void inline_oop(oop o);

  // C calls
  void set_last_Delta_frame_before_call(); // assumes that the return address has not been pushed yet
  void set_last_Delta_frame_after_call(); // assumes that the return address has been pushed already
  void reset_last_Delta_frame();

  void call_C(Label& L);
  void call_C(Label& L, Label& nlrTestPoint);

  void call_C(char* entry, relocInfo::relocType rtype);
  void call_C(char* entry, relocInfo::relocType rtype, Label& nlrTestPoint);

  void call_C(Register entry);
  void call_C(Register entry, Label& nlrTestPoint);

  // C calls to run-time routines with arguments (args are not preserved)
  void call_C(char* entry, Register arg1);
  void call_C(char* entry, Register arg1, Register arg2);
  void call_C(char* entry, Register arg1, Register arg2, Register arg3);
  void call_C(char* entry, Register arg1, Register arg2, Register arg3, Register arg4);

  // Stores
  void store_check(Register obj, Register tmp);

  // Floating-point comparisons
  // To jump conditionally on cc, test FPU status word with mask and
  // jump conditionally using cond.
  static void fpu_mask_and_cond_for(Condition cc, int& mask, Condition& cond);

  // Pop ST (ffree & fincstp combined)
  void fpop();

  // Float-stack depth management is a no-op on x86 (the FPU stack is
  // hardware); the calls exist so the shared interpreter/Floats code can set
  // the generation-time depth uniformly across backends.
  int float_depth() const { return 0; }
  void set_float_depth(int d) {}

  // debugging
  static void print_reg(char* name, oop obj);
  static void inspector(oop edi, oop esi, oop ebp, oop esp, oop ebx, oop edx, oop ecx, oop eax, char* eip);
  void inspect(char* title = NULL);
};
#endif // _ASSEMBLER_X86_HPP
